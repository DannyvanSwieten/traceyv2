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
#include "sops/sop_node.hpp"   // tracey::sops::EmittedActor (used in cook-result slot)
#include "sops/sop_graph.hpp"  // tracey::sops::NodeCookTiming
#include "sops/cook_cache.hpp"

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

    // Map from SOP node uid → emitted actor uid in the scene. Covers
    // object_output, light, and subnet nodes (one actor per SOP node) PLUS
    // the *primary* (instanceIndex == 0) actor for `instance` SOPs — used
    // by external paths (animation overlay, gizmo writeback) that work on
    // a SOP-node basis. For per-emitted-actor lookups during apply_emitted
    // (signatures, scene-object ownership, transform updates across all
    // instances) see m_emitted_actor_to_actor below.
    std::unordered_map<size_t, uint64_t> m_sop_node_to_actor;

    // Map from composite (sourceNodeUid, instanceIndex) → actor uid. Stores
    // every emitted actor, including the N instance actors that share a
    // sourceNodeUid coming out of the `instance` SOP. apply_emitted's
    // per-actor maps (signatures, object names) all use this composite
    // key so they don't collide across instances.
    std::unordered_map<uint64_t, uint64_t> m_emitted_actor_to_actor;

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

    // True if any SOP node carries an animated parameter (channel keys
    // anywhere in the tree). Superset of detect_animated_vop_promotions;
    // also includes keyed translate/rotate/scale on subnets and transform
    // SOPs that apply_animation_at processes.
    bool detect_any_animation() const;

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

    // Persistent per-node geometry caches. Separate instances so the worker
    // thread (set_sop_graph live edits) and the main thread (cook_and_apply
    // for load_scene + export + apply_animation_at re-cooks) don't need
    // synchronization. They store the same kind of data — independent caches
    // just cost a bit of extra memory while the editor's open.
    tracey::sops::CookCache m_worker_cook_cache;
    tracey::sops::CookCache m_main_cook_cache;

    // FNV-1a fingerprint of the previous apply_emitted's input. apply_emitted
    // hashes the new emitted set against this; on a match it skips the scene
    // rebuild + compile_scene entirely. The signature covers per-actor
    // geometry (vertex count + position digest), TRS, name, parent uid,
    // material/light fields — enough that any actor-visible change to the
    // cook output forces a real apply. Reset on every cook that miss-applies
    // (or never matched).
    uint64_t m_last_emitted_signature = 0;
    bool m_has_applied_once = false;

    // Per-actor delta tracking. `structural_sig` covers everything that
    // would force a scene rebuild (geometry digest, name, parent/light
    // bits, material library). `transform_sig` covers only TRS. When all
    // changes vs. the previous cook are transform_sig deltas (no add,
    // remove, or structural change), apply_emitted takes the fast path:
    // update each affected actor's transform in place and ask the engine
    // to refresh ONLY the TLAS — no scene.clear, no BLAS work, no
    // material buffer re-upload.
    struct ActorSig {
        uint64_t transform_sig = 0;
        uint64_t structural_sig = 0;
    };
    // Keyed by `make_actor_key(sourceNodeUid, instanceIndex)` so the
    // `instance` SOP (which emits N actors sharing one sourceNodeUid) can
    // track each instance's signature independently — without this, the
    // last instance in the emitted vector would clobber all earlier ones
    // and the structural-change detector would always misfire.
    std::unordered_map<uint64_t, ActorSig> m_actor_signatures;

    // Per-actor SceneObject name. Keyed by the composite actor key so
    // each instance from the `instance` SOP has its own owner entry (the
    // actual SceneObject name is usually shared across instances via
    // Phase-A content dedup, but the ownership lookup needs to disambiguate
    // each instance for cleanup).
    std::unordered_map<uint64_t, std::string> m_sop_node_object_names;

    // Per-SceneObject refcount + reverse geometry-content map. Together they
    // implement Phase-A GPU instancing: identical Geometry payloads emitted
    // by different SOP nodes (two `primitive_cube`s, a glTF mesh imported
    // twice, etc.) share ONE SceneObject + one vertex/color buffer + one
    // BLAS instead of paying the upload cost N times. The BLAS cache already
    // dedupes BVHs by name+hash; this layer dedupes the SceneObjects that
    // feed those names.
    //
    //   • m_scene_object_refcount[name] = how many actors currently
    //     reference this SceneObject. removeObject only happens when this
    //     drops to 0.
    //   • m_geometry_hash_to_object_name[geomHash] = the name we used last
    //     time we saw this exact content. Re-emitted geometry hits this and
    //     reuses the existing SceneObject.
    std::unordered_map<std::string, size_t> m_scene_object_refcount;
    std::unordered_map<uint64_t, std::string> m_geometry_hash_to_object_name;

    std::mutex m_cook_result_mutex;
    // Pending result delivered from the cook worker to the main thread.
    // Bundles the emitted-actor list + per-node cook timings so the
    // profiler tab can show what the worker just did. Latest-wins.
    struct PendingCookResult {
        std::vector<tracey::sops::EmittedActor> emitted;
        std::vector<tracey::sops::NodeCookTiming> timings;
    };
    std::optional<PendingCookResult> m_pending_cook_result;

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
