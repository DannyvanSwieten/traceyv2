#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "render_engine.hpp"

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

private:
    std::unique_ptr<RenderEngine> m_engine;
    EditorWindow* m_window = nullptr;  // not owned
    std::vector<uint8_t> m_last_render_pixels;
    uint32_t m_last_render_width = 0;
    uint32_t m_last_render_height = 0;
    std::mutex m_mutex;
    BroadcastCallback m_broadcast;
};

}  // namespace tracey_editor
