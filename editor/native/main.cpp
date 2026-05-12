#include "editor_server.hpp"
#include "platform/platform.hpp"
#include "render_engine.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

static fs::path executable_dir() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::path(buf).parent_path();
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) return fs::path(buf).parent_path();
#endif
    return fs::current_path();
}

static std::string find_ui_path() {
    // Dev override: TRACEY_EDITOR_DEV_URL skips the bundled UI lookup so frontend
    // edits hot-reload via Vite without a C++ rebuild. The launch.json
    // "Tracey Editor (Dev)" config sets this.
    if (const char* dev_url = std::getenv("TRACEY_EDITOR_DEV_URL")) {
        if (*dev_url) return dev_url;
    }

    fs::path exe_dir = executable_dir();

    fs::path bundle = exe_dir / ".." / "Resources" / "editor-dist" / "index.html";
    if (fs::exists(bundle)) return fs::canonical(bundle).string();

    fs::path adjacent = exe_dir / "editor-dist" / "index.html";
    if (fs::exists(adjacent)) return adjacent.string();

    return "http://localhost:5173";
}

static fs::path find_shader_dir() {
    // Dev: tracey/examples/scene_renderer/shaders/ (above the build dir)
    // Bundled (.app): Resources/shaders/ (same level as editor-dist)
    fs::path exe_dir = executable_dir();

    fs::path bundled = exe_dir / ".." / "Resources" / "shaders";
    if (fs::exists(bundled)) return fs::canonical(bundled);

    // Search upward for examples/scene_renderer/shaders.
    fs::path cur = exe_dir;
    for (int i = 0; i < 8; ++i) {
        fs::path candidate = cur / "examples" / "scene_renderer" / "shaders";
        if (fs::exists(candidate)) return fs::canonical(candidate);
        if (cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    return fs::path{"shaders"};
}

int main(int /*argc*/, char** /*argv*/) {
    auto window = tracey_editor::create_editor_window();
    if (!window) {
        std::fprintf(stderr, "Failed to create editor window.\n");
        return 1;
    }

    if (!window->create(1280, 720, "Tracey Editor")) {
        std::fprintf(stderr, "Failed to initialize editor window.\n");
        return 1;
    }

    tracey_editor::RenderConfig config;
    config.width = 1280;
    config.height = 720;
    config.shader_dir = find_shader_dir();
    // outputImage is the LDR tonemapped snapshot the resolve shader writes
    // into; the linear running mean lives in the accumulator. We blit
    // outputImage directly to the swapchain — no CPU tonemap needed.
    config.hdr_output = false;
    config.max_samples = 1024;
    config.max_bounces = 8;

    std::printf("Shader dir: %s\n", config.shader_dir.string().c_str());

    std::unique_ptr<tracey_editor::RenderEngine> engine;
    try {
        engine = std::make_unique<tracey_editor::RenderEngine>(config);
        engine->initialize_path_tracer();
        engine->initialize_rasterizer();
        // Install an empty CompiledScene up front so render_tick can draw
        // the reference ground grid before the first import. Without this,
        // a fresh launch with no SOP graph yet has m_compiled_scene == null,
        // and render_tick early-returns leaving the viewport black.
        engine->compile_scene();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Render engine init failed: %s\n", e.what());
        return 1;
    }

    auto server = std::make_unique<tracey_editor::EditorServer>(std::move(engine), window.get());
    server->set_broadcast_callback(
        [&window](const std::string& json) { window->send_to_webview(json); });

    window->set_message_handler([&server](const std::string& json) -> std::string {
        return server->handle_command(json);
    });

    // Continuous render tick: CVDisplayLink fires this on the main thread once
    // per refresh. The server short-circuits until the viewport is active and
    // the scene is compiled.
    window->set_render_tick([&server]() { server->render_tick(); });

    const std::string ui_path = find_ui_path();
    std::printf("Loading UI from: %s\n", ui_path.c_str());
    window->load_ui(ui_path);
    window->show();
    window->start_render_tick();

    while (!window->should_close()) {
        window->poll_events();
    }

    window->stop_render_tick();
    return 0;
}
