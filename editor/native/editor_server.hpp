#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "render_engine.hpp"
#include "viewport_renderer.hpp"

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

    // Camera-orbit state (the Camera class only stores a quaternion).
    float m_camera_yaw = 0.0f;
    float m_camera_pitch = 0.0f;
    bool m_camera_initialized = false;
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

    // Map from SOP object_output node uid → emitted actor uid in the scene
    // (m_engine->scene().actors()). Lets `set_actor_transform` find the
    // upstream SOP node to write back into.
    std::unordered_map<size_t, uint64_t> m_object_output_to_actor;

    // Cook the current SOP graph and rebuild the live scene from the result.
    // Mutex must be held by the caller; main-thread only because it touches
    // Vulkan resources (path tracer recompile).
    void cook_and_apply();
};

}  // namespace tracey_editor
