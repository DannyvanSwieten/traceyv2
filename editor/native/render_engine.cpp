#include "render_engine.hpp"

#include "scene/scene_object.hpp"
#include "graph/graphs/shader_graph/compiler.hpp"
#include "graph/graphs/shader_graph/serialization.hpp"

#include <stdexcept>

namespace tracey_editor {

RenderEngine::RenderEngine(RenderConfig config) : m_config(std::move(config)) {
    // Present-capable device so the editor can construct a swapchain against
    // the same VulkanContext used by the path tracer.
    auto* dev = tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute,
                                     /*enablePresentation=*/true);
    if (!dev)
        throw std::runtime_error("Failed to create rendering device");
    m_device.reset(dev);
    m_scene = std::make_unique<tracey::Scene>();
    m_blas_cache = std::make_unique<tracey::BlasCache>();
}

RenderEngine::~RenderEngine() = default;

void RenderEngine::initialize_path_tracer() {
    // Initial PT size mirrors the configured viewport size — render_engine
    // owners (EditorServer) call set_resolutions() with the inset rect once
    // the viewport reports its real pixel dimensions.
    if (m_pt_width == 0) m_pt_width = m_config.width;
    if (m_pt_height == 0) m_pt_height = m_config.height;

    tracey::PathTracerConfig pt_config;
    pt_config.width = m_pt_width;
    pt_config.height = m_pt_height;
    pt_config.rayGenShader = m_config.shader_dir / "ray_gen.glsl";
    pt_config.hitShader = m_config.shader_dir / "uber_hit.glsl";
    pt_config.missShader = m_config.shader_dir / "sky_miss.glsl";
    pt_config.resolveShader = m_config.shader_dir / "resolve.glsl";
    pt_config.hdrOutput = m_config.hdr_output;
    // One sample per render_tick. Accumulation stops once max_samples is
    // reached (enforced in EditorServer::render_tick).
    pt_config.samplesPerFrame = 1;
    pt_config.maxBounces = m_config.max_bounces;
    pt_config.useMaterialPrograms = true;

    m_path_tracer = std::make_unique<tracey::PathTracer>(m_device.get(), pt_config);

    // Seed the editor with a passthrough graph so the very first
    // get_material_graph from the frontend returns something useful.
    //
    // One MaterialInput, one MaterialOutput, wired port-to-port for the
    // five PBR slots the editor exposes (Albedo, Metallic, Roughness,
    // Emission, Normal). Port indices match materialInputPorts() and
    // materialOutputPorts() in src/graph/graphs/shader_graph/nodes.hpp:
    //   MaterialInput  outputs: P, N, T, V, uv0, uv1, InstanceID,
    //                            Albedo(7), Metallic(8), Roughness(9),
    //                            Emission(10), InNormal(11)
    //   MaterialOutput inputs:  Albedo(0), Metallic(1), Roughness(2),
    //                            Emission(3), Normal(4), Alpha(5),
    //                            IOR(6), Transmission(7)
    if (m_current_graph_json.empty()) {
        m_current_graph_json = R"({
    "version": 1,
    "uid": 0,
    "nodes": [
        {"uid": 1, "kind": "MaterialInput",  "position": [ 80, 60]},
        {"uid": 2, "kind": "MaterialOutput", "position": [420, 60]}
    ],
    "connections": [
        {"from_node": 1, "from_port":  7, "to_node": 2, "to_port": 0},
        {"from_node": 1, "from_port":  8, "to_node": 2, "to_port": 1},
        {"from_node": 1, "from_port":  9, "to_node": 2, "to_port": 2},
        {"from_node": 1, "from_port": 10, "to_node": 2, "to_port": 3},
        {"from_node": 1, "from_port": 11, "to_node": 2, "to_port": 4}
    ]
})";
    }
    // Material programs are now driven by SceneCompiler from per-actor
    // graphs; the seeded passthrough lives inside the compiled scene buffer.
}

void RenderEngine::initialize_rasterizer() {
    tracey::RasterizerConfig cfg;
    cfg.width = m_config.width;
    cfg.height = m_config.height;
    cfg.vertexShader   = m_config.shader_dir / "rasterizer" / "position_only.vert.spv";
    cfg.fragmentShader = m_config.shader_dir / "rasterizer" / "position_only.frag.spv";
    cfg.pointsVertexShader   = m_config.shader_dir / "rasterizer" / "points.vert.spv";
    cfg.pointsFragmentShader = m_config.shader_dir / "rasterizer" / "points.frag.spv";
    cfg.linesVertexShader    = m_config.shader_dir / "rasterizer" / "lines.vert.spv";
    cfg.linesFragmentShader  = m_config.shader_dir / "rasterizer" / "lines.frag.spv";
    cfg.groundVertexShader   = m_config.shader_dir / "rasterizer" / "ground.vert.spv";
    cfg.groundFragmentShader = m_config.shader_dir / "rasterizer" / "ground.frag.spv";
    cfg.gizmoVertexShader    = m_config.shader_dir / "rasterizer" / "gizmo.vert.spv";
    cfg.gizmoFragmentShader  = m_config.shader_dir / "rasterizer" / "gizmo.frag.spv";
    cfg.useDepthBuffer = true;
    cfg.depthTestEnable = true;
    cfg.cullBackFaces = false;  // SOP-cooked geometry can have either winding
    cfg.alphaBlending = false;
    cfg.colorFormat = tracey::ImageFormat::R8G8B8A8Unorm;
    m_rasterizer = std::make_unique<tracey::Rasterizer>(m_device.get(), cfg);
    m_rasterizer->setShowPoints(m_show_points);
    m_rasterizer->setShowEdges(m_show_edges);
    m_rasterizer->setShowGround(m_show_ground);
    // Apply the persisted background colour. The rasterizer's
    // ctor-default matches m_bg_* so this is a no-op on first launch;
    // when the user has set a custom colour and then we recreate the
    // rasterizer (e.g. on viewport resize), it carries through.
    m_rasterizer->setBackgroundColor(m_bg_r, m_bg_g, m_bg_b, m_bg_a);
}

void RenderEngine::set_material_graph_json(const std::string& graph_json) {
    if (!m_path_tracer)
        throw std::runtime_error("Path tracer not initialized");
    // Scratchpad only -- the modal authors a graph that the user later saves
    // to the library and assigns to actors. Live preview during edit would
    // require a separate "preview slot" concept; not in scope yet.
    m_current_graph_json = graph_json;
}

void RenderEngine::set_material_parameter(uint32_t program_id, uint32_t param_idx,
                                          float x, float y, float z, float w) {
    if (!m_path_tracer)
        throw std::runtime_error("Path tracer not initialized");
    m_path_tracer->setMaterialParameter(program_id, param_idx, tracey::Vec4(x, y, z, w));
}

void RenderEngine::compile_scene() {
    // Empty scene (e.g. SOP graph has no ObjectOutput wired yet, or fresh
    // launch before the first import): SceneCompiler::compile would throw on
    // no-geometry, so we install an empty CompiledScene instead of resetting
    // to null. That lets the rasterizer keep drawing the reference ground
    // grid (which doesn't need any scene instances) while we wait for the
    // user to add geometry. The path tracer side checks `instances.empty()`
    // and skips dispatch on its own.
    bool has_geometry = false;
    for (const auto& [_name, obj] : m_scene->objects()) {
        if (obj && obj->vertexCount() > 0) { has_geometry = true; break; }
    }
    if (!has_geometry) {
        m_compiled_scene =
            std::make_unique<tracey::SceneCompiler::CompiledScene>();
        return;
    }

    // Mark all cache entries untouched before the compile; any entry whose
    // SceneObject doesn't appear (by name + matching content hash) in this
    // compile will stay untouched and get evicted below. The cache itself
    // is bypassed when raster-only (SceneCompiler::compile ignores it in
    // that mode) so the markAll/evictUntouched bookkeeping is harmless —
    // the next PT-mode compile will repopulate everything from scratch.
    m_blas_cache->markAllUntouched();
    auto compiled = tracey::SceneCompiler::compile(
        m_device.get(), *m_scene, tracey::BVHConfig{}, m_blas_cache.get(),
        /*buildAccelerationStructures=*/m_build_acceleration_structures);
    m_compiled_scene =
        std::make_unique<tracey::SceneCompiler::CompiledScene>(std::move(compiled));
    m_blas_cache->evictUntouched();

    // Per-actor graphs were aggregated into materialPrograms during compile;
    // push them to the GPU so the hit shader's runMaterialProgram(programId, ...)
    // sees the right code at each programId slot. Skipped in raster-only
    // mode — the rasterizer doesn't run material programs, and the next
    // PT-on compile will upload a fresh program buffer anyway.
    if (m_build_acceleration_structures &&
        m_path_tracer && !m_compiled_scene->materialPrograms.headers().empty()) {
        m_path_tracer->setMaterialPrograms(m_compiled_scene->materialPrograms);
    }
}

void RenderEngine::set_show_points(bool v) {
    m_show_points = v;
    if (m_rasterizer) m_rasterizer->setShowPoints(v);
}

void RenderEngine::set_show_edges(bool v) {
    m_show_edges = v;
    if (m_rasterizer) m_rasterizer->setShowEdges(v);
}

void RenderEngine::set_show_ground(bool v) {
    m_show_ground = v;
    if (m_rasterizer) m_rasterizer->setShowGround(v);
}

void RenderEngine::set_gizmo_visible(bool v) {
    if (m_rasterizer) m_rasterizer->setGizmoVisible(v);
}

void RenderEngine::set_gizmo_anchor(float x, float y, float z, float length) {
    if (m_rasterizer) {
        m_rasterizer->setGizmoAnchor(tracey::Vec3(x, y, z));
        m_rasterizer->setGizmoLength(length);
    }
}

void RenderEngine::set_background_color(float r, float g, float b, float a) {
    m_bg_r = r; m_bg_g = g; m_bg_b = b; m_bg_a = a;
    if (m_rasterizer) m_rasterizer->setBackgroundColor(r, g, b, a);
}

void RenderEngine::render_rasterizer() {
    if (!m_rasterizer || !m_compiled_scene || !m_scene->hasCamera()) return;
    m_rasterizer->render(*m_compiled_scene, m_scene->camera());
}

bool RenderEngine::refresh_tlas_only() {
    if (!m_compiled_scene || !m_compiled_scene->tlas) return false;

    // Walk the scene the same way SceneCompiler does (flatten() then per-actor
    // instances in declaration order). The instance index across this walk
    // must match the previous compile_scene's index 1:1 — that's how the
    // per-instance material indices, program ids, and UV offsets stay
    // correct without rebuilding their buffers.
    auto sceneNodes = m_scene->flatten();
    std::vector<tracey::Tlas::Instance> newInstances;
    newInstances.reserve(m_compiled_scene->instances.size());

    size_t instanceIndex = 0;
    for (const auto &node : sceneNodes) {
        const tracey::Actor *actor = node.actor;
        if (!actor || !actor->visible()) continue;
        const tracey::Mat4 &worldTransform = node.worldTransform;

        for (const auto &sceneInstance : actor->instances()) {
            const std::string &objectRef = sceneInstance.objectRef();
            auto it = m_compiled_scene->objectToBlasIndex.find(objectRef);
            if (it == m_compiled_scene->objectToBlasIndex.end()) continue;

            tracey::Mat4 finalTransform = worldTransform;
            if (sceneInstance.hasLocalTransform()) {
                finalTransform = worldTransform *
                                 sceneInstance.localTransform()->toMatrix();
            }

            tracey::Tlas::Instance instance;
            instance.setTransform(finalTransform);
            instance.blasAddress = it->second;
            if (instanceIndex < m_compiled_scene->instances.size()) {
                // Preserve material/SBT bits from the previous instance —
                // those bookkeeping fields didn't change with the transform.
                const auto &prev = m_compiled_scene->instances[instanceIndex];
                instance.setCustomIndex(prev.instanceCustomIndex());
                instance.setMask(prev.instanceMask());
                instance.setSbtRecordOffset(prev.sbtRecordOffset());
                instance.setInstanceFlags(prev.instanceFlags());
            } else {
                instance.setMask(0xFF);
            }
            newInstances.push_back(instance);
            ++instanceIndex;
        }
    }

    // Topology guard: if instance count drifted (visibility toggled between
    // compile and this refresh, or actor add/remove slipped past the
    // caller's precondition), bail and let the caller fall through to a
    // full compile_scene.
    if (newInstances.size() != m_compiled_scene->instances.size()) return false;

    // Re-collect BLAS pointers from the (still-owned by BlasCache) entries.
    std::vector<const tracey::BottomLevelAccelerationStructure *> blasPtrs;
    blasPtrs.reserve(m_compiled_scene->blases.size());
    for (const auto *b : m_compiled_scene->blases) blasPtrs.push_back(b);

    m_compiled_scene->instances = std::move(newInstances);
    m_compiled_scene->tlas = std::unique_ptr<tracey::TopLevelAccelerationStructure>(
        m_device->createTopLevelAccelerationStructure(
            std::span<const tracey::BottomLevelAccelerationStructure *>(
                blasPtrs.data(), blasPtrs.size()),
            std::span<const tracey::Tlas::Instance>(
                m_compiled_scene->instances.data(),
                m_compiled_scene->instances.size())));
    return true;
}

RenderResult RenderEngine::render_frame(bool clear_accumulation, bool want_pixels) {
    if (!m_path_tracer)
        throw std::runtime_error("Path tracer not initialized");
    if (!m_compiled_scene)
        throw std::runtime_error("Scene not compiled");
    if (!m_scene->hasCamera())
        throw std::runtime_error("Scene has no camera");

    // Empty scene (every actor hidden, only lights/subnet markers, or fresh
    // graph with no object_output): nothing for the path tracer to trace
    // against. Return a zeroed RenderResult instead of crashing — the
    // rasterizer still runs and shows the ground / overlays.
    if (m_compiled_scene->instances.empty())
    {
        const uint32_t width = m_pt_width;
        const uint32_t height = m_pt_height;
        const size_t bytes_per_pixel = m_config.hdr_output ? 16 : 4;
        RenderResult result;
        result.width = width;
        result.height = height;
        result.sample_count = m_path_tracer->sampleCount();
        result.render_time_ms = 0.0;
        if (want_pixels)
        {
            result.pixels.assign(static_cast<size_t>(width) * height * bytes_per_pixel, 0);
        }
        return result;
    }

    const double render_time_ms =
        m_path_tracer->render(*m_compiled_scene, m_scene->camera(),
                              clear_accumulation, want_pixels);

    const uint32_t width = m_path_tracer->width();
    const uint32_t height = m_path_tracer->height();

    RenderResult result;
    result.width = width;
    result.height = height;
    result.sample_count = m_path_tracer->sampleCount();
    result.render_time_ms = render_time_ms;
    if (want_pixels)
    {
        const size_t bytes_per_pixel = m_config.hdr_output ? 16 : 4;
        const size_t buffer_size = static_cast<size_t>(width) * height * bytes_per_pixel;
        result.pixels.resize(buffer_size);
        m_path_tracer->readback(result.pixels.data());
    }
    return result;
}

void RenderEngine::set_resolution(uint32_t width, uint32_t height) {
    // Back-compat: when only one size is given, raster + PT share it.
    set_resolutions(width, height, width, height);
}

void RenderEngine::set_resolutions(uint32_t raster_w, uint32_t raster_h,
                                   uint32_t pt_w, uint32_t pt_h) {
    if (raster_w == 0 || raster_h == 0) return;
    if (pt_w == 0) pt_w = raster_w;
    if (pt_h == 0) pt_h = raster_h;

    const bool raster_changed =
        raster_w != m_config.width || raster_h != m_config.height;
    const bool pt_changed = pt_w != m_pt_width || pt_h != m_pt_height;
    if (!raster_changed && !pt_changed) return;

    m_config.width = raster_w;
    m_config.height = raster_h;
    m_pt_width = pt_w;
    m_pt_height = pt_h;

    if (pt_changed && m_path_tracer) {
        // Recreate the path tracer so its output image, descriptor sets, and
        // accumulator get rebuilt at the new size. Per-actor material graphs
        // are preserved (they live on the actors) and get re-uploaded on the
        // next compile_scene.
        m_path_tracer.reset();
        initialize_path_tracer();
    }
    if (raster_changed && m_rasterizer) {
        m_rasterizer.reset();
        initialize_rasterizer();
    }
}

uint32_t RenderEngine::current_samples() const {
    return m_path_tracer ? m_path_tracer->sampleCount() : 0;
}

uint32_t RenderEngine::max_bounces() const {
    if (m_path_tracer)
        return m_path_tracer->maxBounces();
    return m_config.max_bounces;
}

void RenderEngine::set_max_bounces(uint32_t bounces) {
    m_config.max_bounces = bounces;
    if (m_path_tracer)
        m_path_tracer->setMaxBounces(bounces);
}

}  // namespace tracey_editor
