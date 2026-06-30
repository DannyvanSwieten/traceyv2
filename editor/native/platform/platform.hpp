#pragma once

#include "../input_state.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tracey_editor {

using MessageCallback = std::function<std::string(const std::string& json)>;
using BroadcastCallback = std::function<void(const std::string& json)>;
using ResizeCallback = std::function<void(uint32_t pixel_w, uint32_t pixel_h)>;
using RenderTickCallback = std::function<void()>;

struct FileFilter {
    std::string description;
    std::vector<std::string> extensions;
};

// Platform-agnostic editor window: webview (UI) + GPU viewport surface in a
// single native window. macOS: NSWindow + WKWebView + CAMetalLayer. Windows
// (future): HWND + WebView2 + Vulkan-on-DXGI swap chain.
class EditorWindow {
public:
    virtual ~EditorWindow() = default;

    virtual bool create(uint32_t width, uint32_t height, const char* title) = 0;

    // ── Webview (UI panels) ──
    virtual void load_ui(const std::string& url_or_path) = 0;
    virtual void send_to_webview(const std::string& json) = 0;
    virtual void set_message_handler(MessageCallback cb) = 0;

    // ── GPU viewport surface ──
    // Native surface handle for Vulkan presentation.
    //   macOS:   CAMetalLayer*  (consume via VK_EXT_metal_surface)
    //   Windows: HWND
    virtual void* gpu_surface() = 0;

    // Native display handle (Windows: HINSTANCE; nullptr elsewhere).
    virtual void* gpu_display() { return nullptr; }

    // Viewport dimensions. _pixel_ variants are framebuffer-sized
    // (logical * backing-scale on Retina); use those when sizing GPU
    // resources / driving the swapchain.
    virtual uint32_t viewport_width() const = 0;
    virtual uint32_t viewport_height() const = 0;
    virtual uint32_t viewport_pixel_width() const = 0;
    virtual uint32_t viewport_pixel_height() const = 0;

    // Position the viewport surface within the window (logical points).
    virtual void set_viewport_rect(int32_t x, int32_t y, uint32_t w, uint32_t h) = 0;
    virtual void set_viewport_visible(bool visible) = 0;
    // Fired when the viewport's pixel size changes (e.g. resize, DPI change).
    virtual void set_resize_callback(ResizeCallback cb) = 0;

    // ── Input ──
    virtual InputState& input() = 0;
    virtual void set_viewport_accepts_mouse(bool accept) = 0;

    // ── Render tick ──
    // Called from the platform's display-link callback on the main thread.
    // The host implementation drives this internally; the editor just plugs
    // in its render function here.
    virtual void set_render_tick(RenderTickCallback cb) = 0;
    virtual void start_render_tick() = 0;
    virtual void stop_render_tick() = 0;

    // ── Window lifecycle ──
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual bool is_visible() const = 0;
    virtual void poll_events() = 0;
    virtual bool should_close() const = 0;

    // ── Native dialogs ──
    virtual std::string open_folder_dialog(const char* title) = 0;
    virtual std::string open_file_dialog(const char* title,
                                         const std::vector<FileFilter>& filters) = 0;
    virtual std::string save_file_dialog(const char* title,
                                         const char* default_name,
                                         const std::vector<FileFilter>& filters) = 0;
    // Single-line text prompt (e.g. "Project name"). Returns the entered text, or
    // empty if the user cancelled.
    virtual std::string prompt_text(const char* title, const char* message,
                                    const char* default_value) = 0;
};

std::unique_ptr<EditorWindow> create_editor_window();

}  // namespace tracey_editor
