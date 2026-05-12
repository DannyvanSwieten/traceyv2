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
    if (m_current_graph_json.empty()) {
        m_current_graph_json = R"({
    "version": 1,
    "uid": 0,
    "nodes": [
        {"uid": 1,  "kind": "InputAttribute", "op": "LoadInputAlbedo",    "position": [80, 60]},
        {"uid": 2,  "kind": "Output",         "op": "WriteAlbedo",        "position": [380, 60]},
        {"uid": 3,  "kind": "InputAttribute", "op": "LoadInputMetallic",  "position": [80, 160]},
        {"uid": 4,  "kind": "Output",         "op": "WriteMetallic",      "position": [380, 160]},
        {"uid": 5,  "kind": "InputAttribute", "op": "LoadInputRoughness", "position": [80, 260]},
        {"uid": 6,  "kind": "Output",         "op": "WriteRoughness",     "position": [380, 260]},
        {"uid": 7,  "kind": "InputAttribute", "op": "LoadInputEmission",  "position": [80, 360]},
        {"uid": 8,  "kind": "Output",         "op": "WriteEmission",      "position": [380, 360]},
        {"uid": 9,  "kind": "InputAttribute", "op": "LoadInputNormal",    "position": [80, 460]},
        {"uid": 10, "kind": "Output",         "op": "WriteNormal",        "position": [380, 460]}
    ],
    "connections": [
        {"from_node": 1, "from_port": 0, "to_node": 2,  "to_port": 0},
        {"from_node": 3, "from_port": 0, "to_node": 4,  "to_port": 0},
        {"from_node": 5, "from_port": 0, "to_node": 6,  "to_port": 0},
        {"from_node": 7, "from_port": 0, "to_node": 8,  "to_port": 0},
        {"from_node": 9, "from_port": 0, "to_node": 10, "to_port": 0}
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
    cfg.useDepthBuffer = true;
    cfg.depthTestEnable = true;
    cfg.cullBackFaces = false;  // SOP-cooked geometry can have either winding
    cfg.alphaBlending = false;
    cfg.colorFormat = tracey::ImageFormat::R8G8B8A8Unorm;
    m_rasterizer = std::make_unique<tracey::Rasterizer>(m_device.get(), cfg);
    m_rasterizer->setShowPoints(m_show_points);
    m_rasterizer->setShowEdges(m_show_edges);
    m_rasterizer->setShowGround(m_show_ground);
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

    auto compiled = tracey::SceneCompiler::compile(m_device.get(), *m_scene);
    m_compiled_scene =
        std::make_unique<tracey::SceneCompiler::CompiledScene>(std::move(compiled));

    // Per-actor graphs were aggregated into materialPrograms during compile;
    // push them to the GPU so the hit shader's runMaterialProgram(programId, ...)
    // sees the right code at each programId slot.
    if (m_path_tracer && !m_compiled_scene->materialPrograms.headers().empty()) {
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

void RenderEngine::render_rasterizer() {
    if (!m_rasterizer || !m_compiled_scene || !m_scene->hasCamera()) return;
    m_rasterizer->render(*m_compiled_scene, m_scene->camera());
}

RenderResult RenderEngine::render_frame(bool clear_accumulation) {
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
        result.pixels.assign(static_cast<size_t>(width) * height * bytes_per_pixel, 0);
        return result;
    }

    const double render_time_ms =
        m_path_tracer->render(*m_compiled_scene, m_scene->camera(), clear_accumulation);

    const uint32_t width = m_path_tracer->width();
    const uint32_t height = m_path_tracer->height();
    const size_t bytes_per_pixel = m_config.hdr_output ? 16 : 4;
    const size_t buffer_size = static_cast<size_t>(width) * height * bytes_per_pixel;

    RenderResult result;
    result.pixels.resize(buffer_size);
    m_path_tracer->readback(result.pixels.data());
    result.width = width;
    result.height = height;
    result.sample_count = m_path_tracer->sampleCount();
    result.render_time_ms = render_time_ms;
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
