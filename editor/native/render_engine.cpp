#include "render_engine.hpp"

#include "scene/gltf_loader.hpp"
#include "scene/scene_object.hpp"

#include <stdexcept>

namespace tracey_editor {

RenderEngine::RenderEngine(RenderConfig config) : m_config(std::move(config)) {
    auto* dev = tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute);
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
    pt_config.rayGenShader = m_config.shader_dir / "ray_gen.isf";
    pt_config.hitShader = m_config.shader_dir / "diffuse_hit.isf";
    pt_config.missShader = m_config.shader_dir / "sky_miss.isf";
    pt_config.resolveShader = m_config.shader_dir / "resolve.isf";
    pt_config.hdrOutput = m_config.hdr_output;
    pt_config.samplesPerFrame = m_config.samples_per_frame;
    pt_config.maxBounces = m_config.max_bounces;

    m_path_tracer = std::make_unique<tracey::PathTracer>(m_device.get(), pt_config);
}

void RenderEngine::compile_scene() {
    auto compiled = tracey::SceneCompiler::compile(m_device.get(), *m_scene);
    m_compiled_scene =
        std::make_unique<tracey::SceneCompiler::CompiledScene>(std::move(compiled));
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
    m_config.width = width;
    m_config.height = height;
    m_path_tracer.reset();
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
