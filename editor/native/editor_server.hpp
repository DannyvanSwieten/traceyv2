#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "render_engine.hpp"
#include "viewport_renderer.hpp"
#include "sops/sop_node.hpp"  // tracey::sops::EmittedActor (used in cook-result slot)

namespace tracey {
    namespace sops {
        class SopGraph;
    }
}

namespace tracey_editor {

class EditorWindow;

class EditorServer {
public:
    EditorServer(std::unique_ptr<RenderEngine> engine, EditorWindow* window);
    ~EditorServer();

    EditorServer(const EditorServer&) = delete;
    EditorServer& operator=(const EditorServer&) = delete;

    using BroadcastCallback = std::function<void(const std::string& json)>;
    void set_broadcast_callback(BroadcastCallback cb);

    // Synchronous request → response. JSON in, JSON out.
    std::string handle_command(const std::string& json_request);

    void broadcast(const std::string& message);

    // Driven by the platform's display-link callback on the main thread.
    // No-op until the frontend has reported a viewport rect and the scene has
    // been compiled.
    void render_tick();

private:
    // Lazily build the ViewportRenderer once we know the viewport pixel size.
    // Recreates on size change.
    void ensure_viewport_renderer(uint32_t pixel_w, uint32_t pixel_h);

    // Read InputState, update the scene camera, and signal accumulation reset
    // on movement. Returns true if anything changed.
    bool update_camera_from_input(double dt);

    std::unique_ptr<RenderEngine> m_engine;
    EditorWindow* m_window = nullptr;  // not owned
    std::unique_ptr<ViewportRenderer> m_viewport;
    uint32_t m_viewport_pixel_w = 0;
    uint32_t m_viewport_pixel_h = 0;
    bool m_viewport_active = false;

    // Orbital camera state. The viewport navigation tumbles/pans/dollies
    // around `m_orbit_pivot_*`; the Camera's position and rotation are derived
    // from this state each frame the user navigates.
    float m_orbit_yaw = 0.0f;
    float m_orbit_pitch = 0.0f;
    float m_orbit_distance = 5.0f;
    float m_orbit_pivot_x = 0.0f;
    float m_orbit_pivot_y = 0.0f;
    float m_orbit_pivot_z = 0.0f;
    bool m_orbit_initialized = false;
    // Currently-selected actor; pivot follows this actor's world position
    // when selection changes.
    std::optional<uint64_t> m_selected_actor_id;

    // Per-SOP-source-node display flag, applied to the emitted actor in
    // apply_emitted so visibility survives a re-cook. Keyed by SOP source
    // node uid (the same key used by m_sop_node_to_actor); entries with
    // value=true are also stored explicitly so the map round-trips cleanly
    // through a save/load cycle if we ever want to persist it.
    std::unordered_map<size_t, bool> m_sop_node_visible;
    bool m_clear_next_frame = true;
    double m_last_tick_time = 0.0;

    std::vector<uint8_t> m_last_render_pixels;
    uint32_t m_last_render_width = 0;
    uint32_t m_last_render_height = 0;
    std::mutex m_mutex;
    BroadcastCallback m_broadcast;

    // Scene-level SOP graph. The Houdini-style /obj network: cooking it
    // produces the list of actors the path tracer renders. All actor
    // creation/edits flow through here.
    std::unique_ptr<tracey::sops::SopGraph> m_sop_graph;

    // Map from SOP node uid → emitted actor uid in the scene
    // (m_engine->scene().actors()). Covers both object_output nodes (the
    // primary geometry-bearing actors) and subnet nodes (transform-only
    // parent actors created from EmittedActor markers). Lets
    // `set_actor_transform` and the keyframe-override path find the source
    // SOP node to write back into.
    std::unordered_map<size_t, uint64_t> m_sop_node_to_actor;

    // ── Timeline / playback ─────────────────────────────────────────────
    // The playhead lives natively. render_tick advances `current_time` while
    // playing, samples animated SOP parameters, and pushes the result onto
    // live actors via the existing transform path. Time is in seconds; the
    // UI converts to/from frames using `fps` so changing fps doesn't
    // invalidate keys.
    enum class LoopMode : uint8_t { Once, Loop, PingPong };
    struct TimelineState {
        double fps = 24.0;
        int frame_start = 1;
        int frame_end = 240;
        double current_time = 0.0;
        bool playing = false;
        LoopMode loop = LoopMode::Loop;
        // PingPong direction (+1 forward, -1 backward). Internal.
        double pingpong_dir = 1.0;
    } m_timeline;

    // Set whenever the live override layer needs to re-evaluate (seek, key
    // edit, cook completion). Cleared after apply_animation_at runs.
    bool m_timeline_dirty = true;

    // Last wall-clock time we broadcast a timeline_tick (rate-limit ~30 Hz so
    // render_tick at 60+ fps doesn't flood the message bus).
    double m_last_timeline_broadcast = 0.0;

    // Sample animated SOP parameters at `time` and override the corresponding
    // live actors' transforms. No-op for non-animated parameters. Mutex must
    // be held; main-thread only.
    void apply_animation_at(double time);

    // Advance the playhead by `dt` seconds, applying loop semantics. Returns
    // true if `current_time` changed.
    bool advance_playhead(double dt);

    // Cook the current SOP graph and rebuild the live scene from the result.
    // Mutex must be held by the caller; main-thread only because it touches
    // Vulkan resources (path tracer recompile). Used by the synchronous
    // load_scene path; the live editing path (set_sop_graph) goes through the
    // async cook worker below instead.
    void cook_and_apply();

    // Apply a previously-cooked emitted-actor list to the live scene + path
    // tracer. Mutex must be held; main-thread only.
    void apply_emitted(std::vector<tracey::sops::EmittedActor>&& emitted);

    // ── Async SOP cook worker ──
    // Cooking can be slow (e.g. large glTF imports); doing it on the message
    // thread blocks the UI. The worker thread owns a private SopGraph copy
    // for the duration of each cook; m_sop_graph stays canonical for the
    // message thread. Latest-wins: a new set_sop_graph while a cook is
    // running just overwrites the pending request.
    void cook_worker_loop();
    // Time defaults to current playhead. Cooks driven by graph edits
    // (set_sop_graph) and structural mutations should use the default so
    // the cook samples animated params at the current frame; the auto
    // re-cook path for VOP-promotion animation in render_tick explicitly
    // passes the current time to keep latest-wins coherent.
    void post_cook_request(std::string graph_json, double time);
    void drain_cook_result();  // called from render_tick on the main thread

    // Walk the canonical SOP graph (recursing into subnet inner graphs) and
    // return true iff any attribute_vop's promoted host param is animated.
    // Recomputed after every cook so the auto re-cook in render_tick stays
    // accurate as the user promotes / demotes / keys params.
    bool detect_animated_vop_promotions() const;

    std::thread m_cook_thread;
    std::mutex m_cook_request_mutex;
    std::condition_variable m_cook_request_cv;
    // Worker request: serialized root SopGraph + playhead time. Latest-wins
    // — both fields get overwritten by each post.
    struct CookRequest {
        std::string graph_json;
        double      time = 0.0;
    };
    std::optional<CookRequest> m_pending_cook_request;
    // Most recently pushed root graph JSON, kept under m_mutex. Reused by
    // the auto re-cook in render_tick (for VOP-promotion animation) — no
    // round trip to the frontend needed.
    std::string m_last_pushed_graph_json;
    // Set true after every cook completion if the resulting graph contains
    // at least one attribute_vop with at least one animated promoted host
    // param. Gates the auto re-cook on time change.
    bool m_has_animated_vop_promotions = false;
    bool m_cook_shutdown = false;

    std::mutex m_cook_result_mutex;
    std::optional<std::vector<tracey::sops::EmittedActor>> m_pending_cook_result;

    // ── Video export ────────────────────────────────────────────────────
    // The export worker drives a frame-accurate offline render: seek →
    // cook_and_apply → render N samples → append to AVAssetWriter. While it
    // runs, render_tick() early-returns so the live viewport doesn't fight
    // for the GPU. Only one export at a time.
    struct VideoExportRequest {
        std::string path;
        int frame_start = 1;
        int frame_end = 1;
        double fps = 24.0;
        int samples_per_frame = 64;
        int max_bounces = 0;   // 0 = leave the engine's current setting alone
        int width = 0;         // 0 = use current viewport (path tracer) resolution
        int height = 0;
        std::string codec;     // "h264" | "prores"
    };
    std::atomic<bool> m_export_in_progress{false};
    std::atomic<bool> m_export_cancel{false};
    std::thread m_export_thread;

    void export_video_loop(VideoExportRequest req);
};

}  // namespace tracey_editor
