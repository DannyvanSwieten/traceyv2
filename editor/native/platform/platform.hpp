#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tracey_editor {

using MessageCallback = std::function<std::string(const std::string& json)>;
using BroadcastCallback = std::function<void(const std::string& json)>;
using ResizeCallback = std::function<void(uint32_t w, uint32_t h)>;

struct FileFilter {
    std::string description;
    std::vector<std::string> extensions;
};

class EditorWindow {
public:
    virtual ~EditorWindow() = default;

    virtual bool create(uint32_t width, uint32_t height, const char* title) = 0;

    virtual void load_ui(const std::string& url_or_path) = 0;
    virtual void send_to_webview(const std::string& json) = 0;
    virtual void set_message_handler(MessageCallback cb) = 0;
    virtual void set_resize_callback(ResizeCallback cb) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual bool is_visible() const = 0;
    virtual void poll_events() = 0;
    virtual bool should_close() const = 0;

    virtual std::string open_folder_dialog(const char* title) = 0;
    virtual std::string open_file_dialog(const char* title,
                                         const std::vector<FileFilter>& filters) = 0;
    virtual std::string save_file_dialog(const char* title,
                                         const char* default_name,
                                         const std::vector<FileFilter>& filters) = 0;
};

std::unique_ptr<EditorWindow> create_editor_window();

}  // namespace tracey_editor
