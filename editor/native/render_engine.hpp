#pragma once

#include "scene/scene.hpp"
#include "scene/scene_compiler.hpp"
#include "rendering/path_tracer.hpp"
#include "device/device.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tracey_editor {

struct RenderConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    std::filesystem::path shader_dir;
    bool hdr_output = false;
    uint32_t samples_per_frame = 16;
    uint32_t max_bounces = 8;
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

    tracey::Scene& scene() { return *m_scene; }
    const tracey::Scene& scene() const { return *m_scene; }

    void compile_scene();
    RenderResult render_frame(bool clear_accumulation);

    void load_gltf(const std::string& path);

    std::pair<uint32_t, uint32_t> resolution() const {
        return {m_config.width, m_config.height};
    }
    void set_resolution(uint32_t width, uint32_t height);

    uint32_t samples_per_frame() const;
    void set_samples_per_frame(uint32_t samples);
    uint32_t max_bounces() const;
    void set_max_bounces(uint32_t bounces);

    bool path_tracer_ready() const { return m_path_tracer != nullptr; }
    bool compiled_scene_ready() const { return m_compiled_scene != nullptr; }

private:
    RenderConfig m_config;
    std::unique_ptr<tracey::Device> m_device;
    std::unique_ptr<tracey::Scene> m_scene;
    std::unique_ptr<tracey::PathTracer> m_path_tracer;
    std::unique_ptr<tracey::SceneCompiler::CompiledScene> m_compiled_scene;
};

}  // namespace tracey_editor
