#pragma once

#include "scene/scene.hpp"
#include "scene/scene_compiler.hpp"
#include "scene/blas_cache.hpp"
#include "path_tracer/api/path_tracer.hpp"
#include "rendering/rasterizer.hpp"
#include "device/device.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace tracey_editor {

struct RenderConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    std::filesystem::path shader_dir;
    bool hdr_output = false;
    // Path-tracer accumulation cap. The editor dispatches one sample per
    // render_tick and stops when sampleCount() reaches this value, leaving
    // the accumulator on screen. Reset to 0 happens automatically on camera
    // / scene / settings changes via m_clear_next_frame.
    uint32_t max_samples = 1024;
    uint32_t max_bounces = 8;
    // Path tracer backend: "auto" | "wavefront" | "metal" | "vulkan_rt" |
    // "cpu". Overridable at launch via the TRACEY_PT_BACKEND env var.
    std::string pt_backend = "auto";
    // EXR-export mode: the path tracer also emits AOV layers and writes the
    // beauty image as LINEAR (float, no tonemap). Off for the viewport; the
    // EXR sequence export flips it on (and forces hdr/float output) via
    // set_export_aovs(), which recreates the PT.
    bool export_aovs = false;
};

struct RenderResult {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sample_count = 0;
    double render_time_ms = 0.0;
};

class RenderEngine {
public:
    explicit RenderEngine(RenderConfig config);
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    void initialize_path_tracer();
    void initialize_rasterizer();

    tracey::Scene& scene() { return *m_scene; }
    const tracey::Scene& scene() const { return *m_scene; }

    void compile_scene();

    // R4 motion blur: attach shutter-close instance poses (parallel to the
    // live compiled scene's `instances`) and bump the revision so the
    // path-tracer backend rebuilds with the motion AS. No-op when the counts
    // mismatch or there's no compiled scene.
    void set_motion_end_instances(std::vector<tracey::Tlas::Instance> endInstances);

    // Controls whether subsequent compile_scene() calls build BLAS / TLAS
    // and upload the material programs. Off → the rasterizer still works
    // (vertex + color buffers are uploaded), but the path tracer is
    // intentionally non-functional. The editor flips this with the PT
    // inset preview toggle; flipping it back on triggers an explicit
    // recompile so the BVH is rebuilt. Defaults to true so headless
    // callers (smoke tests, scene_renderer example) keep working.
    bool build_acceleration_structures() const { return m_build_acceleration_structures; }
    void set_build_acceleration_structures(bool v) { m_build_acceleration_structures = v; }
    // `want_pixels` controls whether the GPU output is copied back to
    // CPU into RenderResult::pixels. The live viewport composites
    // path_tracer()->outputImage() directly from the GPU and passes
    // false, avoiding a fullscreen vkCmdCopyImageToBuffer plus a
    // mapForReading stall + heap alloc every tick. The export / JSON
    // render_frame command paths pass true.
    RenderResult render_frame(bool clear_accumulation, bool want_pixels = true);

    // Run a rasterizer pass into m_rasterizer's color image. Cheap (no
    // accumulation, no readback). The image is presented straight to the
    // viewport via ViewportRenderer::present_composite.
    void render_rasterizer();

    std::pair<uint32_t, uint32_t> resolution() const {
        return {m_config.width, m_config.height};
    }
    std::pair<uint32_t, uint32_t> pt_resolution() const {
        return {m_pt_width, m_pt_height};
    }
    void set_resolution(uint32_t width, uint32_t height);
    // Set raster (full viewport) and PT (inset) sizes independently. The
    // path tracer's output is sized to the inset, saving most of the ray
    // budget while the rasterizer drives the full window.
    void set_resolutions(uint32_t raster_w, uint32_t raster_h,
                         uint32_t pt_w, uint32_t pt_h);

    uint32_t max_samples() const { return m_config.max_samples; }
    void set_max_samples(uint32_t n) { m_config.max_samples = n; }
    uint32_t current_samples() const;
    uint32_t max_bounces() const;
    void set_max_bounces(uint32_t bounces);

    // Path-tracer backend selection at runtime: "auto" | "metal" | "cpu".
    // Recreates the PT (the façade rebuilds output resources for the new
    // backend's output kind) and re-pushes material programs, mirroring
    // set_export_aovs(). No-op if unchanged.
    const std::string& pt_backend() const { return m_config.pt_backend; }
    void set_pt_backend(const std::string& backend);

    // Toggle EXR-export mode (AOV layers + linear/float beauty). Recreates the
    // path tracer (output image, accumulator, AOV buffers) and re-pushes the
    // material programs, mirroring set_resolutions(). The EXR sequence export
    // brackets its frame loop with set_export_aovs(true)/(false).
    bool export_aovs() const { return m_config.export_aovs; }
    void set_export_aovs(bool v);

    bool path_tracer_ready() const { return m_path_tracer != nullptr; }
    bool rasterizer_ready() const { return m_rasterizer != nullptr; }
    bool compiled_scene_ready() const { return m_compiled_scene != nullptr; }

    // Render-thread handoff: snapshot the current compiled scene as a
    // shared_ptr so the worker can keep rendering after the main thread
    // swaps in a freshly compiled scene from apply_emitted. The
    // returned pointer is safe to dereference for the lifetime of the
    // shared_ptr (no external lock needed once held).
    std::shared_ptr<const tracey::SceneCompiler::CompiledScene>
        compiled_scene_snapshot() const { return m_compiled_scene; }

    // Run the rasterizer against an explicit scene snapshot + camera.
    // Used by the render worker thread so the live scene swap doesn't
    // race the in-flight render. Returns the wall-clock render time in
    // milliseconds, or 0 if there's nothing to render.
    // `expectedGeneration` is the scene_generation() captured when the snapshot
    // was taken. If a compile_scene has happened since (generation advanced),
    // the snapshot's raw Buffer*/BLAS pointers may dangle — evictUntouched()
    // frees cache entries for geometry no longer in the scene — so we skip the
    // render rather than bind freed buffers. Checked under the shared GPU lock,
    // mutually exclusive with the compile that bumps the generation.
    double render_rasterizer_with(
        const tracey::SceneCompiler::CompiledScene &scene,
        const tracey::Camera &camera,
        uint64_t expectedGeneration);

    // Accumulate ONE path-tracer sample against an explicit scene snapshot +
    // camera, into the PT's own output image/accumulator (no readback — the
    // live viewport composites outputImage() straight off the GPU). Used by the
    // PT worker thread so the CPU backend's synchronous per-sample compute runs
    // off the main thread. `clear` resets the Welford accumulator first.
    // `expectedGeneration`: same stale-snapshot guard as render_rasterizer_with.
    // Returns the sample wall-clock in ms, or 0 if there's nothing to trace.
    double render_path_tracer_with(
        const tracey::SceneCompiler::CompiledScene &scene,
        const tracey::Camera &camera,
        bool clear,
        uint64_t expectedGeneration);

    // Monotonic counter bumped by every compile_scene(). A render snapshot
    // captures this; the render workers compare it to skip stale snapshots whose
    // BlasCache buffers may have been evicted. Read by render_tick under m_mutex
    // (consistent with compile, which runs under m_mutex) and by the workers
    // under the shared GPU lock (consistent with compile's unique lock).
    uint64_t scene_generation() const {
        return m_scene_generation.load(std::memory_order_acquire);
    }

    // Rebuild ONLY the TLAS from the current scene's actor transforms.
    // Keeps the BLAS cache, material buffer, UV buffer, and per-instance
    // material/program-id bookkeeping untouched. Caller must guarantee:
    //   • scene topology (actor add/remove/visibility) is identical to
    //     the previous compile, otherwise instance ordering drifts and
    //     the per-instance material/program indices line up wrong.
    //   • only `actor->transform()` values changed since last compile.
    // Returns false if the precondition fails or no compiled scene exists.
    bool refresh_tlas_only();
    // True only when the compiled scene has at least one TLAS instance; the
    // rasterizer can still draw the reference ground grid without geometry,
    // so this gates the path tracer (which needs a BVH) specifically.
    bool has_renderable_geometry() const {
        return m_compiled_scene && !m_compiled_scene->instances.empty();
    }

    // Profiler-tab counters. Cheap inline accessors that pull directly
    // from the compiled scene; safe to call every render_tick. Zero
    // when no scene has been compiled yet.
    size_t total_triangles() const {
        return m_compiled_scene ? m_compiled_scene->totalTriangles : 0;
    }
    size_t total_instances() const {
        return m_compiled_scene ? m_compiled_scene->instances.size() : 0;
    }
    size_t total_bvh_nodes() const {
        return m_compiled_scene ? m_compiled_scene->totalNodes : 0;
    }

    tracey::Device& device() { return *m_device; }
    tracey::PathTracer* path_tracer() { return m_path_tracer.get(); }
    tracey::Rasterizer* rasterizer() { return m_rasterizer.get(); }

    bool show_points() const { return m_show_points; }
    void set_show_points(bool v);

    bool show_edges() const { return m_show_edges; }
    void set_show_edges(bool v);

    bool show_ground() const { return m_show_ground; }
    void set_show_ground(bool v);

    // Translate-gizmo overlay (three colored world-axis lines anchored at
    // the selected actor). The selection state lives on the editor side;
    // this just exposes "where + visible".
    void set_gizmo_visible(bool v);
    void set_gizmo_anchor(float x, float y, float z, float length);

    // Rasterizer viewport background color. Linear [0,1] components;
    // the rasterizer's colorFormat handles the sRGB encoding for
    // display. Default is the previous hardcoded blue-grey so saved
    // scenes look the same on first launch.
    void background_color(float &r, float &g, float &b, float &a) const {
        r = m_bg_r; g = m_bg_g; b = m_bg_b; a = m_bg_a;
    }
    void set_background_color(float r, float g, float b, float a);

    // Material graph (single global graph for now -- shared by all instances).
    // The editor stores the raw JSON so client-side metadata (node positions,
    // etc.) survives a round-trip; the engine deserializes only when compiling.
    const std::string& get_material_graph_json() const { return m_current_graph_json; }
    void set_material_graph_json(const std::string& graph_json);
    void set_material_parameter(uint32_t program_id, uint32_t param_idx, float x, float y, float z, float w);

private:
    RenderConfig m_config;
    // Path tracer renders into the inset rect; raster size = m_config.width/height.
    uint32_t m_pt_width = 0;
    uint32_t m_pt_height = 0;
    bool m_show_points = false;
    bool m_show_edges = false;
    bool m_show_ground = true;
    bool m_build_acceleration_structures = true;
    float m_bg_r = 0.2f;
    float m_bg_g = 0.3f;
    float m_bg_b = 0.4f;
    float m_bg_a = 1.0f;
    std::unique_ptr<tracey::Device> m_device;
    std::unique_ptr<tracey::Scene> m_scene;
    std::unique_ptr<tracey::PathTracer> m_path_tracer;
    std::unique_ptr<tracey::Rasterizer> m_rasterizer;
    // Guards GPU-resource lifetime across our worker threads. The two render
    // entry points — render_rasterizer_with (rasterizer worker) and
    // render_path_tracer_with (PT worker) — take a SHARED lock: they read the
    // resources and don't free each other's, so they run concurrently. Anything
    // that recreates/rebuilds GPU resources — set_resolutions, compile_scene,
    // set_pt_backend — takes a UNIQUE lock, so it waits for every in-flight
    // render to finish and blocks new ones until it's done.
    //
    // Without this, a worker could be binding/submitting/mapping buffers and
    // command pools while the main thread (a resolution switch, a cook-apply)
    // frees and recreates them — producing vkFreeCommandBuffers / vkUnmapMemory
    // THREADING ERRORs and use-after-free. Mutators are rare and a render is
    // short, so the exclusion is cheap. (No method takes both lock kinds, and no
    // guarded method calls another, so there's no recursive-acquire deadlock.)
    std::shared_mutex m_gpu_mutex;
    // Bumped on every compile_scene() (under m_gpu_mutex's unique lock). See
    // scene_generation() — the stale-snapshot guard for the render workers.
    std::atomic<uint64_t> m_scene_generation{0};
    // shared_ptr so the render worker thread can hold a snapshot of the
    // current scene (via shared_ptr copy) while the main thread swaps in
    // a freshly compiled scene from apply_emitted. Once all snapshots
    // drop, the old CompiledScene destructs on the last holder's thread.
    std::shared_ptr<tracey::SceneCompiler::CompiledScene> m_compiled_scene;
    // Per-object BLAS + vertex/color buffer cache that survives between
    // compile_scene() calls. SceneCompiler queries it on each compile so a
    // recook with unchanged geometry doesn't rebuild BVHs or re-upload
    // vertex data. Cleared by clear_blas_cache().
    std::unique_ptr<tracey::BlasCache> m_blas_cache;

    // Raw JSON for the active material graph. Empty until first set or first
    // initialise (which seeds a passthrough). Source of truth across IPC.
    std::string m_current_graph_json;
};

}  // namespace tracey_editor
