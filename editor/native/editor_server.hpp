#pragma once

#include <json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
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
#include "scene/camera.hpp"    // tracey::Camera (held by value in RenderRequest)
#include "sops/sop_node.hpp"   // tracey::sops::EmittedActor (used in cook-result slot)
#include "sops/sop_graph.hpp"  // tracey::sops::NodeCookTiming
#include "sops/cook_cache.hpp"
#include "dops/eval_context.hpp"   // tracey::dops::SopGeometryProvider

namespace tracey {
    namespace sops {
        class SopGraph;
    }
    namespace dops {
        class DopGraph;
    }
    namespace vops {
        namespace codegen {
            class VopComputeDispatcher;
        }
    }
    namespace sops {
        namespace codegen {
            class CopyToPointsCompute;
            class TransformCompute;
            class MergeCompute;
        }
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
    // Per-domain IPC command modules (editor_server_cmds_*.cpp). Each
    // returns the response string when it handled `cmd`, std::nullopt
    // otherwise. Called by handle_command with m_mutex held, inside its
    // try block.
    std::optional<std::string> handle_scene_commands(const std::string& cmd, const nlohmann::json& req);
    std::optional<std::string> handle_render_commands(const std::string& cmd, const nlohmann::json& req);
    std::optional<std::string> handle_material_commands(const std::string& cmd, const nlohmann::json& req);
    std::optional<std::string> handle_graph_commands(const std::string& cmd, const nlohmann::json& req);
    std::optional<std::string> handle_timeline_commands(const std::string& cmd, const nlohmann::json& req);
    std::optional<std::string> handle_io_commands(const std::string& cmd, const nlohmann::json& req);

    // Re-wire DopGraph's SopGeometryProvider after every m_dop_graph
    // reset (constructor + load_scene). The provider itself is owned by
    // the EditorServer and points at m_main_cook_cache; only the raw
    // pointer inside m_dop_graph needs refreshing when the unique_ptr
    // gets replaced. Implemented in editor_server.cpp where the full
    // DopGraph type is in scope.
    void wire_dop_sop_provider();

    // Lazily build the ViewportRenderer once we know the viewport pixel size.
    // Recreates on size change.
    void ensure_viewport_renderer(uint32_t pixel_w, uint32_t pixel_h);

    // Reapply the PT render size based on (fullscreen flag, override w/h,
    // viewport size). Called whenever any of those inputs changes. Skips
    // when the viewport hasn't been initialised yet — the next
    // ensure_viewport_renderer() will pick up the right size.
    void apply_pt_resolution();

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

    // Whether the live viewport composites the path-traced inset preview
    // on top of the rasterizer. Off by default — the PT pass is by far
    // the most expensive thing render_tick does, and most editor
    // sessions spend their time on geometry / material edits where the
    // rasterizer's preview is sufficient. The user opts in with the
    // set_pt_preview command (and the future toolbar toggle). When
    // false, the path tracer is never dispatched and the composite
    // falls back to a plain rasterizer present.
    bool m_pt_preview_enabled = false;

    // When true (and m_pt_preview_enabled also true), the path-tracer
    // output takes the entire viewport instead of the top-right PiP
    // inset. Used by the Render workspace so the user gets a real
    // dedicated final-look view. Resolution is bumped to viewport res
    // on transition so the PT isn't upscaled-blitted.
    bool m_pt_fullscreen = false;

    // Optional fixed render resolution for the path tracer in fullscreen
    // (Render workspace) mode. 0 = "match viewport pixel size". When
    // non-zero, the PT renders at this exact res and the viewport blit
    // upscales / downscales to fit. Lets the user pick "1080p preview
    // regardless of window size" so final-look samples match what an
    // Export would produce.
    uint32_t m_pt_render_w = 0;
    uint32_t m_pt_render_h = 0;

    // True while the JS side is running a modal grab (G/R/S transform).
    // Two effects per render_tick:
    //   1. update_camera_from_input is skipped so dragging doesn't orbit
    //      the camera underneath the user's intent.
    //   2. The native pointer state is broadcast back to JS as
    //      `viewport_pointer` events so the grab math can run there —
    //      otherwise the WebView never sees the events because the
    //      CAMetalLayer captures them first.
    bool m_viewport_grab_active = false;
    // Track the last broadcast values so we only fire on change (no
    // need to spam JS with identical-position events every tick).
    float m_last_broadcast_mouse_x = 0.0f;
    float m_last_broadcast_mouse_y = 0.0f;
    bool  m_last_broadcast_mouse_left  = false;
    bool  m_last_broadcast_mouse_right = false;

    // Current project folder. Empty when no project is open (fresh
    // launch, or the user is editing without having saved/opened
    // anything). When set, project-scoped material lookups read from
    // `m_project_dir/materials/<name>.json` first, falling back to the
    // global library; saves can target either scope. save_scene /
    // load_scene set this implicitly to the file's parent so the legacy
    // single-file flow gets a sane materials folder for free.
    std::filesystem::path m_project_dir;

    std::vector<uint8_t> m_last_render_pixels;
    uint32_t m_last_render_width = 0;
    uint32_t m_last_render_height = 0;

    // Profiler-tab render-stats broadcast state. Exponentially-
    // smoothed dt drives a steady FPS readout (raw 1/dt jitters under
    // load); broadcast is throttled to ~4 Hz so the message bus
    // doesn't pay 60 events/sec for a counter the UI only needs to
    // refresh at human-readable cadence.
    double m_smoothed_dt = 1.0 / 60.0;
    double m_last_render_stats_broadcast = 0.0;
    double m_last_render_time_ms = 0.0;
    // Per-tick wall-clock buckets, EMA-smoothed against the previous
    // measurement to keep the profiler readout from twitching. tick_ms
    // is the total render_tick body (excluding the try_lock skip),
    // and the others slice that into the dominant phases so a missing
    // bucket immediately surfaces an unmeasured cost.
    double m_smoothed_tick_ms    = 0.0;
    double m_smoothed_rebuild_ms = 0.0;
    double m_smoothed_raster_ms  = 0.0;
    double m_smoothed_present_ms = 0.0;
    std::mutex m_mutex;
    BroadcastCallback m_broadcast;

    // ── Render worker thread ───────────────────────────────────────────
    // The main thread used to call the rasterizer directly inside
    // render_tick, blocking on the GPU fence wait for the duration of
    // the dispatch. That froze the UI under heavy raster loads and
    // serialised everything Vulkan-shaped (cook, present) behind it.
    //
    // Now: render_tick snapshots scene + camera under m_mutex, hands
    // the snapshot to a worker thread via a latest-wins mailbox, and
    // immediately moves on to present whatever the worker most-recently
    // finished. The worker calls Rasterizer::render — which has been
    // restructured to release the process-wide queue mutex around its
    // fence wait, so the main thread's present can submit while the
    // worker is waiting for GPU completion.
    //
    // Single-buffered output image race: a present that overlaps an
    // in-progress raster is still safe because both submissions
    // serialize through vulkanQueueMutex and Vulkan's per-queue
    // ordering ensures the present-blit executes AFTER the raster
    // it's reading from. CPU-side, main and worker hold the queue
    // mutex only during command-buffer record + submit (microseconds);
    // both fence waits run lock-free.
    struct RenderRequest
    {
        // Held by shared_ptr so the worker keeps the scene alive even
        // if apply_emitted swaps in a new CompiledScene on the main
        // thread mid-render.
        std::shared_ptr<const tracey::SceneCompiler::CompiledScene> scene;
        tracey::Camera camera;
    };

    std::thread             m_render_thread;
    std::mutex              m_render_mutex;
    std::condition_variable m_render_cv;
    // Latest-wins mailbox. The main thread overwrites whenever it
    // builds a fresher snapshot; the worker drains-and-runs.
    std::optional<RenderRequest> m_pending_render_request;
    bool m_render_thread_should_exit = false;
    // Goes up by one each time the worker finishes a render. The main
    // thread reads this to decide whether the rasterizer output image
    // has at least one valid frame to present (the very first ticks
    // before the worker has produced anything, we skip present).
    std::atomic<uint64_t> m_render_frames_completed{0};
    // Last raster wall-clock reported by the worker. Plain atomic so
    // the profiler broadcast can read it without taking the render
    // mutex. Stored in milliseconds.
    std::atomic<double> m_worker_raster_ms{0.0};

    void render_thread_main();
    // Stop + join the render worker. Safe to call from the destructor
    // even if the thread was never started (no-op).
    void stop_render_thread();

    // Scene-level SOP graph. The Houdini-style /obj network: cooking it
    // produces the list of actors the path tracer renders. All actor
    // creation/edits flow through here.
    std::unique_ptr<tracey::sops::SopGraph> m_sop_graph;

    // Top-level DOP graph — the simulation network, peer of the root
    // SOP graph (NOT nested inside it). Cooks one frame at a time,
    // caching SimState per frame. SOPs read sim output via a `dop_import`
    // SOP that names a frame. Always non-null after construction; an
    // empty DopGraph is a no-op cook.
    std::unique_ptr<tracey::dops::DopGraph> m_dop_graph;

    // GPU dispatcher for VOP graphs. Created lazily at construction
    // against the RenderEngine's Vulkan compute device. attribute_vop
    // SOPs and pop_force DOPs reach this through the process-wide
    // VopComputeDispatcher::getGlobal() accessor and try a GPU dispatch
    // before falling through to the CPU evaluator. Null when the
    // backend isn't compute-capable (host fallback only).
    std::unique_ptr<tracey::vops::codegen::VopComputeDispatcher> m_vop_dispatcher;
    std::unique_ptr<tracey::sops::codegen::CopyToPointsCompute> m_ctp_dispatcher;
    std::unique_ptr<tracey::sops::codegen::TransformCompute> m_xform_dispatcher;
    std::unique_ptr<tracey::sops::codegen::MergeCompute> m_merge_dispatcher;

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
        // Playback policy. Off (default) = "async": render_tick advances
        // the playhead by wall-clock dt every tick and posts a cook
        // request at the new time; cooks finishing late silently drop
        // intermediate frames so the UI never blocks. On = "every
        // frame": advance_playhead is suppressed entirely and instead
        // drain_cook_result advances by exactly 1/fps after each cook
        // is applied, then posts the next cook. Playback speed becomes
        // cook throughput rather than wall-clock, but no frame is
        // skipped. The UI thread still tics at vsync in both modes —
        // never blocking.
        bool frame_locked = false;
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
    bool advance_playhead_by(double dt);

    // Project-folder helpers. project_material_dir() returns
    // `m_project_dir/materials` and is only meaningful when a project
    // is open. resolve_material_path() returns the path the engine
    // should actually read for a material with the given library
    // name: project-local wins over global so a project can shadow a
    // same-named global material. Returns an empty path when neither
    // scope has the file.
    std::filesystem::path project_material_dir() const;
    std::filesystem::path resolve_material_path(const std::string &name) const;

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
    bool detect_animated_sop_params() const;

    // True iff the canonical SOP graph contains at least one dop_import
    // node (recursing into subnets). Refreshed after every cook + on every
    // set_sop_graph, since either can introduce one. Gates the playhead-
    // driven DOP re-cook in render_tick so a project without particles
    // doesn't pay the cook cost on every scrub.
    bool detect_dop_imports() const;
    bool m_has_dop_imports = false;

    // Collect per-dop_import stamp pairs from the canonical graphs at the
    // given playhead time. Cooks the DopGraph to the corresponding frame
    // (cheap when already cached). Caller must hold m_mutex. Returns an
    // empty vector when m_has_dop_imports is false.
    std::vector<std::pair<size_t, tracey::Geometry>>
        collect_dop_stamps(double time);

    // True if any SOP node carries an animated parameter (channel keys
    // anywhere in the tree). Superset of detect_animated_sop_params;
    // also includes keyed translate/rotate/scale on subnets and transform
    // SOPs that apply_animation_at processes.
    bool detect_any_animation() const;

    std::thread m_cook_thread;
    std::mutex m_cook_request_mutex;
    std::condition_variable m_cook_request_cv;
    // Worker request: serialized root SopGraph + playhead time. Latest-wins
    // — every field gets overwritten by each post.
    //
    // `dop_stamps` is the side channel for dop_import SOPs. The canonical
    // graph's dop_import nodes carry a `m_stamped` Geometry that the cook()
    // returns; serializing the graph drops that buffer, so we pass it
    // alongside the JSON and the worker re-stamps after deserialize. Empty
    // when the graph has no dop_import nodes (the common case).
    struct CookRequest {
        std::string graph_json;
        double      time = 0.0;
        std::vector<std::pair<size_t, tracey::Geometry>> dop_stamps;
    };
    std::optional<CookRequest> m_pending_cook_request;
    // Most recently pushed root graph JSON, kept under m_mutex. Reused by
    // the auto re-cook in render_tick (for VOP-promotion animation) — no
    // round trip to the frontend needed.
    std::string m_last_pushed_graph_json;
    // Set true after every cook completion if the resulting graph contains
    // at least one attribute_vop with at least one animated promoted host
    // param. Gates the auto re-cook on time change.
    bool m_has_animated_sop_params = false;
    bool m_cook_shutdown = false;

    // Persistent per-node geometry caches. Separate instances so the worker
    // thread (set_sop_graph live edits) and the main thread (cook_and_apply
    // for load_scene + export + apply_animation_at re-cooks) don't need
    // synchronization. They store the same kind of data — independent caches
    // just cost a bit of extra memory while the editor's open.
    tracey::sops::CookCache m_worker_cook_cache;
    tracey::sops::CookCache m_main_cook_cache;

    // SOP-graph back-reference for the DOP graph's pop_source(emit_mode=
    // geometry). The DOP cook always runs on the main thread (under
    // m_mutex, inside collect_dop_stamps), so reading the main cache is
    // race-free. Lifetime: owned by EditorServer; DopGraph holds a raw
    // pointer set once at construction.
    class SopProviderImpl : public tracey::dops::SopGeometryProvider {
    public:
        explicit SopProviderImpl(const tracey::sops::CookCache *cache) : m_cache(cache) {}
        const tracey::Geometry *lookupCookedGeometry(uint64_t sopUid) const override {
            if (!m_cache) return nullptr;
            return m_cache->findOutput(static_cast<size_t>(sopUid));
        }
    private:
        const tracey::sops::CookCache *m_cache;
    };
    SopProviderImpl m_sop_provider{&m_main_cook_cache};

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
        // Output mode. "video" (default) → AVFoundation movie via codec.
        // "exr" → a multi-layer OpenEXR sequence (linear beauty + AOVs), one
        // file per frame named "<path-stem>.NNNN.exr".
        std::string format = "video";
        // EXR only: run the OIDN denoiser on the beauty using the albedo +
        // normal AOVs as guides before writing. Ignored without TRACEY_WITH_OIDN.
        bool denoise = false;
    };
    std::atomic<bool> m_export_in_progress{false};
    std::atomic<bool> m_export_cancel{false};
    std::thread m_export_thread;

    void export_video_loop(VideoExportRequest req);

    // ── Still render ────────────────────────────────────────────────────
    // A single offline frame at an arbitrary resolution → one PNG (LDR,
    // tonemapped) or one multi-layer EXR (linear beauty + AOVs, optionally
    // denoised). Renders the CURRENT scene + camera (no timeline seek, no
    // re-cook). Shares the export worker/flags — a still and a sequence export
    // are mutually exclusive, and render_tick() pauses for both.
    struct RenderStillRequest {
        std::string path;
        int width = 0;        // 0 = use current viewport (path tracer) resolution
        int height = 0;
        int samples = 64;
        int max_bounces = 0;  // 0 = leave the engine's current setting alone
        std::string format = "png"; // "png" (LDR) | "exr" (linear + AOVs)
        bool denoise = false; // EXR only; ignored without TRACEY_WITH_OIDN
    };
    void render_still_loop(RenderStillRequest req);
};

}  // namespace tracey_editor
