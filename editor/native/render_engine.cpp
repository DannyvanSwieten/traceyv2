#include "render_engine.hpp"

#include "scene/gltf_loader.hpp"
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
    tracey::PathTracerConfig pt_config;
    pt_config.width = m_config.width;
    pt_config.height = m_config.height;
    pt_config.rayGenShader = m_config.shader_dir / "ray_gen.glsl";
    pt_config.hitShader = m_config.shader_dir / "uber_hit.glsl";
    pt_config.missShader = m_config.shader_dir / "sky_miss.glsl";
    pt_config.resolveShader = m_config.shader_dir / "resolve.glsl";
    pt_config.hdrOutput = m_config.hdr_output;
    pt_config.samplesPerFrame = m_config.samples_per_frame;
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

RenderResult RenderEngine::render_frame(bool clear_accumulation) {
    if (!m_path_tracer)
        throw std::runtime_error("Path tracer not initialized");
    if (!m_compiled_scene)
        throw std::runtime_error("Scene not compiled");
    if (!m_scene->hasCamera())
        throw std::runtime_error("Scene has no camera");

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

void RenderEngine::load_gltf(const std::string& path) {
    auto loaded = tracey::GltfLoader::loadFromFile(path);
    if (!loaded)
        throw std::runtime_error("Failed to load GLTF file: " + path);

    m_scene->clear();

    for (const auto& actor : loaded->actors()) {
        auto* new_actor = m_scene->createActor();
        new_actor->setName(actor->name());
        new_actor->setTransform(actor->transform());
        for (const auto& instance : actor->instances())
            new_actor->addInstance(instance);
    }

    for (const auto& [name, obj] : loaded->objects()) {
        tracey::SceneObject copy = *obj;
        m_scene->addObject(name, std::move(copy));
    }

    if (loaded->hasCamera())
        m_scene->setCamera(loaded->camera());

    for (const auto& [id, tex] : loaded->embeddedTextures()) {
        tracey::EmbeddedTexture copy = tex;
        m_scene->addEmbeddedTexture(id, std::move(copy));
    }
}

void RenderEngine::set_resolution(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == m_config.width && height == m_config.height) return;
    m_config.width = width;
    m_config.height = height;
    // Recreate the path tracer so its output image, descriptor sets, and
    // accumulator get rebuilt at the new size. The active scene's per-actor
    // graphs are preserved (they live on the actors) and get re-uploaded the
    // next time compile_scene runs.
    if (m_path_tracer) {
        m_path_tracer.reset();
        initialize_path_tracer();
    }
}

uint32_t RenderEngine::samples_per_frame() const {
    if (m_path_tracer)
        return m_path_tracer->samplesPerFrame();
    return m_config.samples_per_frame;
}

void RenderEngine::set_samples_per_frame(uint32_t samples) {
    m_config.samples_per_frame = samples;
    if (m_path_tracer)
        m_path_tracer->setSamplesPerFrame(samples);
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
