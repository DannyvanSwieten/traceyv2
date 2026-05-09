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

    tracey::Device& device() { return *m_device; }
    tracey::PathTracer* path_tracer() { return m_path_tracer.get(); }

    // Material graph (single global graph for now -- shared by all instances).
    // The editor stores the raw JSON so client-side metadata (node positions,
    // etc.) survives a round-trip; the engine deserializes only when compiling.
    const std::string& get_material_graph_json() const { return m_current_graph_json; }
    void set_material_graph_json(const std::string& graph_json);
    void set_material_parameter(uint32_t program_id, uint32_t param_idx, float x, float y, float z, float w);

private:
    RenderConfig m_config;
    std::unique_ptr<tracey::Device> m_device;
    std::unique_ptr<tracey::Scene> m_scene;
    std::unique_ptr<tracey::PathTracer> m_path_tracer;
    std::unique_ptr<tracey::SceneCompiler::CompiledScene> m_compiled_scene;

    // Raw JSON for the active material graph. Empty until first set or first
    // initialise (which seeds a passthrough). Source of truth across IPC.
    std::string m_current_graph_json;
};

}  // namespace tracey_editor
