#include "editor_server.hpp"

#include "platform/platform.hpp"
#include "scene_state.hpp"

#include "scene/actor.hpp"
#include "scene/camera.hpp"
#include "scene/material_instance.hpp"
#include "scene/scene.hpp"
#include "scene/scene_instance.hpp"
#include "scene/scene_object.hpp"
#include "scene/transform.hpp"

#include "rendering/path_tracer.hpp"

#include <glm/gtc/quaternion.hpp>

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <stdexcept>

using nlohmann::json;

namespace tracey_editor {

namespace {

// ─── JSON helpers (mirror the wire format of the old Tauri editor) ──────────

json vec3_to_json(const tracey::Vec3& v) {
    return {{"x", v.x}, {"y", v.y}, {"z", v.z}};
}

tracey::Vec3 vec3_from_json(const json& j) {
    return {j.at("x").get<float>(), j.at("y").get<float>(), j.at("z").get<float>()};
}

json quat_to_json(const tracey::Quaternion& q) {
    return {{"w", q.w}, {"x", q.x}, {"y", q.y}, {"z", q.z}};
}

tracey::Quaternion quat_from_json(const json& j) {
    return {j.at("w").get<float>(), j.at("x").get<float>(), j.at("y").get<float>(),
            j.at("z").get<float>()};
}

json transform_to_json(const tracey::Transform& t) {
    return {
        {"position", vec3_to_json(t.position())},
        {"rotation", quat_to_json(t.rotation())},
        {"scale", vec3_to_json(t.scale())},
    };
}

tracey::Transform transform_from_json(const json& j) {
    tracey::Transform t;
    t.setPosition(vec3_from_json(j.at("position")));
    t.setRotation(quat_from_json(j.at("rotation")));
    t.setScale(vec3_from_json(j.at("scale")));
    return t;
}

json camera_to_json(const tracey::Camera& c) {
    return {
        {"position", vec3_to_json(c.position())},
        {"rotation", quat_to_json(c.rotation())},
        {"fov", c.fov()},
        {"near_plane", c.nearPlane()},
        {"far_plane", c.farPlane()},
        {"aspect_ratio", c.aspectRatio()},
    };
}

tracey::Camera camera_from_json(const json& j) {
    tracey::Camera c;
    c.setPosition(vec3_from_json(j.at("position")));
    c.setRotation(quat_from_json(j.at("rotation")));
    c.setFov(j.at("fov").get<float>());
    c.setNearPlane(j.at("near_plane").get<float>());
    c.setFarPlane(j.at("far_plane").get<float>());
    c.setAspectRatio(j.at("aspect_ratio").get<float>());
    return c;
}

json actor_to_json(const tracey::Actor& a) {
    json children = json::array();
    for (size_t uid : a.children()) children.push_back(uid);
    return {
        {"id", a.getUid()},
        {"name", a.name()},
        {"transform", transform_to_json(a.transform())},
        {"children", std::move(children)},
    };
}

json instance_to_json(const tracey::SceneInstance& inst) {
    json result = {
        {"object_ref", inst.objectRef()},
        {"shader_id", inst.material().shaderId()},
        {"has_local_transform", inst.hasLocalTransform()},
    };
    if (inst.hasLocalTransform() && inst.localTransform().has_value()) {
        result["local_transform"] = transform_to_json(*inst.localTransform());
    } else {
        result["local_transform"] = nullptr;
    }
    return result;
}

json mesh_info_to_json(const tracey::SceneObject& obj) {
    return {
        {"name", obj.name()},
        {"vertex_count", obj.vertexCount()},
        {"triangle_count", obj.triangleCount()},
        {"has_indices", obj.hasIndices()},
        {"has_normals", obj.hasNormals()},
        {"has_uvs", obj.hasUvs()},
    };
}

json texture_info_to_json(const std::string& id, const tracey::EmbeddedTexture& tex) {
    return {
        {"id", id},
        {"width", tex.width},
        {"height", tex.height},
        {"channels", tex.channels},
        {"mime_type", tex.mimeType},
    };
}

// ─── Primitive creation helper (mirrors c_api/tracey_api.cpp) ───────────────

uint64_t add_primitive_actor(tracey::Scene& scene, const std::string& name,
                             tracey::SceneObject&& obj) {
    scene.addObject(name, std::move(obj));

    auto* actor = scene.createActor();
    actor->setName(name);

    tracey::MaterialInstance material("pbr");
    material.setVec3("baseColor", tracey::Vec3(0.8f, 0.8f, 0.8f));
    material.setFloat("metallic", 0.0f);
    material.setFloat("roughness", 0.5f);

    actor->addInstance(tracey::SceneInstance(name, material));
    return actor->getUid();
}

// ─── Response envelope ──────────────────────────────────────────────────────

std::string ok_response(const json& data) {
    return json{{"ok", true}, {"data", data}}.dump();
}

std::string ok_response_null() {
    return json{{"ok", true}, {"data", nullptr}}.dump();
}

std::string err_response(const std::string& message) {
    return json{{"ok", false}, {"error", message}}.dump();
}

}  // namespace

EditorServer::EditorServer(std::unique_ptr<RenderEngine> engine, EditorWindow* window)
    : m_engine(std::move(engine)), m_window(window) {}

EditorServer::~EditorServer() = default;

void EditorServer::set_broadcast_callback(BroadcastCallback cb) {
    m_broadcast = std::move(cb);
}

void EditorServer::broadcast(const std::string& message) {
    if (m_broadcast) m_broadcast(message);
}

void EditorServer::ensure_viewport_renderer(uint32_t pixel_w, uint32_t pixel_h) {
    if (!m_window || pixel_w == 0 || pixel_h == 0) return;

    if (!m_viewport) {
        try {
            m_viewport = std::make_unique<ViewportRenderer>(
                m_engine->device(), m_window->gpu_surface(), m_window->gpu_display(),
                pixel_w, pixel_h);
            m_viewport_pixel_w = pixel_w;
            m_viewport_pixel_h = pixel_h;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[viewport] failed to create renderer: %s\n", e.what());
            m_viewport.reset();
        }
        return;
    }

    if (pixel_w != m_viewport_pixel_w || pixel_h != m_viewport_pixel_h) {
        m_viewport->resize(pixel_w, pixel_h);
        m_viewport_pixel_w = pixel_w;
        m_viewport_pixel_h = pixel_h;
    }
}

bool EditorServer::update_camera_from_input(double dt) {
    if (!m_window) return false;
    auto& input = m_window->input();

    if (!m_engine->scene().hasCamera()) {
        // Default camera if scene didn't ship one.
        tracey::Camera cam;
        cam.setPosition({0.0f, 0.0f, 3.0f});
        m_engine->scene().setCamera(cam);
    }
    tracey::Camera cam = m_engine->scene().camera();

    if (!m_camera_initialized) {
        // Decompose existing rotation into yaw/pitch (assumes camera was set
        // looking down -Z with no roll).
        m_camera_yaw = 0.0f;
        m_camera_pitch = 0.0f;
        m_camera_initialized = true;
    }

    bool changed = false;

    constexpr float MOUSE_SENSITIVITY = 0.005f;
    if (input.mouse_left && (input.mouse_dx != 0.0f || input.mouse_dy != 0.0f)) {
        m_camera_yaw -= input.mouse_dx * MOUSE_SENSITIVITY;
        m_camera_pitch -= input.mouse_dy * MOUSE_SENSITIVITY;
        m_camera_pitch = std::clamp(m_camera_pitch,
                                    -1.5707f + 0.01f, 1.5707f - 0.01f);
        // Compose yaw (around world Y) then pitch (around local X).
        glm::quat qyaw = glm::angleAxis(m_camera_yaw, glm::vec3(0, 1, 0));
        glm::quat qpitch = glm::angleAxis(m_camera_pitch, glm::vec3(1, 0, 0));
        cam.setRotation(qyaw * qpitch);
        changed = true;
    }
    input.mouse_dx = 0.0f;
    input.mouse_dy = 0.0f;

    constexpr float MOVE_SPEED = 4.0f;  // units / sec
    const float step = MOVE_SPEED * static_cast<float>(dt);
    glm::vec3 fwd = cam.forward();
    glm::vec3 right = cam.right();
    glm::vec3 pos = cam.position();
    if (input.key_w) { pos += fwd * step; changed = true; }
    if (input.key_s) { pos -= fwd * step; changed = true; }
    if (input.key_d) { pos += right * step; changed = true; }
    if (input.key_a) { pos -= right * step; changed = true; }
    if (input.key_q || input.key_space) { pos += glm::vec3(0, 1, 0) * step; changed = true; }
    if (input.key_e || input.key_shift) { pos += glm::vec3(0, -1, 0) * step; changed = true; }

    if (input.scroll_dy != 0.0f) {
        pos += fwd * (input.scroll_dy * 0.05f);
        changed = true;
    }
    input.scroll_dx = 0.0f;
    input.scroll_dy = 0.0f;

    if (changed) {
        cam.setPosition(pos);
        if (m_viewport_pixel_h > 0)
            cam.setAspectRatio(static_cast<float>(m_viewport_pixel_w) /
                               static_cast<float>(m_viewport_pixel_h));
        m_engine->scene().setCamera(cam);
    }
    return changed;
}

void EditorServer::render_tick() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_viewport_active || !m_window) return;
    if (!m_engine->path_tracer_ready() || !m_engine->compiled_scene_ready()) return;

    ensure_viewport_renderer(m_window->viewport_pixel_width(),
                             m_window->viewport_pixel_height());
    if (!m_viewport) return;

    // dt for camera movement.
    using clock = std::chrono::steady_clock;
    const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    const double dt = m_last_tick_time > 0.0 ? std::min(now - m_last_tick_time, 0.1) : 1.0 / 60.0;
    m_last_tick_time = now;

    if (update_camera_from_input(dt)) m_clear_next_frame = true;

    try {
        const bool clear = m_clear_next_frame;
        m_clear_next_frame = false;
        auto result = m_engine->render_frame(clear);
        m_last_render_width = result.width;
        m_last_render_height = result.height;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewport] render failed: %s\n", e.what());
        return;
    }

    auto* tracer = m_engine->path_tracer();
    if (!tracer) return;
    auto* output = tracer->outputImage();
    if (!output) return;

    // outputImage holds the linear HDR running average (resolve.isf writes
    // pre-tonemap). The swapchain is created with an sRGB format, so the GPU
    // does the linear→sRGB gamma encode on blit and the OS does final
    // tonemap-by-clamp at display. No CPU step needed; values >1 saturate
    // (acceptable for an editor preview).
    if (!m_viewport->present(output)) {
        // Swapchain became invalid (resize, occlusion, etc.) — recreate next tick.
        m_viewport_pixel_w = 0;
        m_viewport_pixel_h = 0;
    }
}

std::string EditorServer::handle_command(const std::string& json_request) {
    json req;
    try {
        req = json::parse(json_request);
    } catch (const std::exception& e) {
        return err_response(std::string{"JSON parse error: "} + e.what());
    }

    const std::string cmd = req.value("cmd", std::string{});
    if (cmd.empty()) return err_response("Missing 'cmd' field");

    std::lock_guard<std::mutex> lock(m_mutex);

    try {
        // ── Scene management ──
        if (cmd == "create_actor") {
            const auto name = req.value("name", std::string{});
            auto* actor = m_engine->scene().createActor();
            if (!name.empty()) actor->setName(name);
            return ok_response(actor->getUid());
        }
        if (cmd == "delete_actor") {
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            const auto* actor = m_engine->scene().getActor(id);
            const bool existed = actor != nullptr;
            if (existed) m_engine->scene().removeActor(id);
            return ok_response(existed);
        }
        if (cmd == "get_all_actors") {
            json arr = json::array();
            for (const auto& a : m_engine->scene().actors()) arr.push_back(actor_to_json(*a));
            return ok_response(arr);
        }
        if (cmd == "get_actor") {
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            const auto* a = m_engine->scene().getActor(id);
            if (!a) return ok_response_null();
            return ok_response(actor_to_json(*a));
        }
        if (cmd == "set_actor_transform") {
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            auto* a = m_engine->scene().getActor(id);
            if (!a) return ok_response(false);
            a->setTransform(transform_from_json(req.at("transform")));
            m_clear_next_frame = true;
            return ok_response(true);
        }
        if (cmd == "set_actor_name") {
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            auto* a = m_engine->scene().getActor(id);
            if (!a) return ok_response(false);
            a->setName(req.at("name").get<std::string>());
            return ok_response(true);
        }
        if (cmd == "set_camera") {
            m_engine->scene().setCamera(camera_from_json(req.at("camera")));
            return ok_response_null();
        }
        if (cmd == "get_camera") {
            if (!m_engine->scene().hasCamera()) {
                return ok_response(camera_to_json(tracey::Camera{}));
            }
            return ok_response(camera_to_json(m_engine->scene().camera()));
        }
        if (cmd == "add_child") {
            const uint64_t parent_id = req.at("parent_id").get<uint64_t>();
            const uint64_t child_id = req.at("child_id").get<uint64_t>();
            auto* parent = m_engine->scene().getActor(parent_id);
            auto* child = m_engine->scene().getActor(child_id);
            if (!parent || !child) return ok_response(false);
            parent->addChild(child);
            return ok_response(true);
        }
        if (cmd == "remove_child") {
            const uint64_t parent_id = req.at("parent_id").get<uint64_t>();
            const uint64_t child_id = req.at("child_id").get<uint64_t>();
            auto* parent = m_engine->scene().getActor(parent_id);
            if (!parent) return ok_response(false);
            parent->removeChild(child_id);
            return ok_response(true);
        }

        // ── Scene resource queries ──
        if (cmd == "get_actor_instances") {
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            json arr = json::array();
            const auto* a = m_engine->scene().getActor(id);
            if (a) {
                for (const auto& inst : a->instances()) arr.push_back(instance_to_json(inst));
            }
            return ok_response(arr);
        }
        if (cmd == "get_mesh_names") {
            json arr = json::array();
            for (const auto& [name, _obj] : m_engine->scene().objects()) arr.push_back(name);
            return ok_response(arr);
        }
        if (cmd == "get_mesh_info") {
            const auto name = req.at("name").get<std::string>();
            const auto* obj = m_engine->scene().getObject(name);
            if (!obj) return err_response("Mesh not found: " + name);
            return ok_response(mesh_info_to_json(*obj));
        }
        if (cmd == "get_all_meshes") {
            json arr = json::array();
            for (const auto& [_name, obj] : m_engine->scene().objects())
                arr.push_back(mesh_info_to_json(*obj));
            return ok_response(arr);
        }
        if (cmd == "get_texture_ids") {
            json arr = json::array();
            for (const auto& [id, _tex] : m_engine->scene().embeddedTextures())
                arr.push_back(id);
            return ok_response(arr);
        }
        if (cmd == "get_texture_info") {
            const auto id = req.at("id").get<std::string>();
            const auto* tex = m_engine->scene().getEmbeddedTexture(id);
            if (!tex) return err_response("Texture not found: " + id);
            return ok_response(texture_info_to_json(id, *tex));
        }
        if (cmd == "get_all_textures") {
            json arr = json::array();
            for (const auto& [id, tex] : m_engine->scene().embeddedTextures())
                arr.push_back(texture_info_to_json(id, tex));
            return ok_response(arr);
        }

        // ── Primitive creation ──
        if (cmd == "add_primitive") {
            const auto name = req.at("name").get<std::string>();
            const auto& params = req.at("params");
            const auto type = params.at("type").get<std::string>();

            uint64_t uid = UINT64_MAX;
            auto& scene = m_engine->scene();
            if (type == "cube") {
                const float size = params.value("size", 1.0f);
                uid = add_primitive_actor(scene, name, tracey::SceneObject::createCube(size));
            } else if (type == "sphere") {
                const float radius = params.value("radius", 1.0f);
                const uint32_t segments = params.value("segments", 16u);
                const uint32_t rings = params.value("rings", 16u);
                uid = add_primitive_actor(
                    scene, name, tracey::SceneObject::createSphere(radius, segments, rings));
            } else if (type == "torus") {
                const float major_r = params.value("major_radius", 1.0f);
                const float minor_r = params.value("minor_radius", 0.3f);
                const uint32_t major_seg = params.value("major_segments", 32u);
                const uint32_t minor_seg = params.value("minor_segments", 16u);
                uid = add_primitive_actor(
                    scene, name,
                    tracey::SceneObject::createTorus(major_r, minor_r, major_seg, minor_seg));
            } else if (type == "plane") {
                const float width = params.value("width", 1.0f);
                const float depth = params.value("depth", 1.0f);
                uid = add_primitive_actor(scene, name,
                                          tracey::SceneObject::createPlane(width, depth));
            } else if (type == "cylinder") {
                const float radius = params.value("radius", 0.5f);
                const float height = params.value("height", 1.0f);
                const uint32_t segments = params.value("segments", 32u);
                uid = add_primitive_actor(
                    scene, name, tracey::SceneObject::createCylinder(radius, height, segments));
            } else if (type == "cone") {
                const float radius = params.value("radius", 0.5f);
                const float height = params.value("height", 1.0f);
                const uint32_t segments = params.value("segments", 32u);
                uid = add_primitive_actor(
                    scene, name, tracey::SceneObject::createCone(radius, height, segments));
            } else {
                return err_response("Unknown primitive type: " + type);
            }
            const auto* a = scene.getActor(uid);
            if (!a) return err_response("Primitive creation failed");
            return ok_response(actor_to_json(*a));
        }

        // ── Rendering ──
        if (cmd == "render_frame") {
            // Update camera before rendering.
            m_engine->scene().setCamera(camera_from_json(req.at("camera")));
            const bool clear = req.value("clear_accumulation", false);
            if (clear || !m_engine->compiled_scene_ready()) m_engine->compile_scene();
            auto result = m_engine->render_frame(clear);
            m_last_render_pixels = std::move(result.pixels);
            m_last_render_width = result.width;
            m_last_render_height = result.height;
            return ok_response({
                {"width", result.width},
                {"height", result.height},
                {"sample_count", result.sample_count},
                {"render_time_ms", result.render_time_ms},
            });
        }
        if (cmd == "get_render_pixels" || cmd == "get_render_pixels_base64") {
            // Both commands return base64 — sending a JSON array of bytes for a
            // 1280×720 frame is multi-MB of ASCII per call. The frontend api
            // wrapper decodes to Uint8Array.
            if (m_last_render_pixels.empty()) return err_response("No render available");
            static const char tab[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const auto& v = m_last_render_pixels;
            std::string out;
            out.reserve(((v.size() + 2) / 3) * 4);
            size_t i = 0;
            for (; i + 3 <= v.size(); i += 3) {
                uint32_t n = (uint32_t(v[i]) << 16) | (uint32_t(v[i + 1]) << 8) | uint32_t(v[i + 2]);
                out += tab[(n >> 18) & 0x3F];
                out += tab[(n >> 12) & 0x3F];
                out += tab[(n >> 6) & 0x3F];
                out += tab[n & 0x3F];
            }
            if (i < v.size()) {
                uint32_t n = uint32_t(v[i]) << 16;
                if (i + 1 < v.size()) n |= uint32_t(v[i + 1]) << 8;
                out += tab[(n >> 18) & 0x3F];
                out += tab[(n >> 12) & 0x3F];
                out += (i + 1 < v.size()) ? tab[(n >> 6) & 0x3F] : '=';
                out += '=';
            }
            return ok_response(out);
        }
        if (cmd == "compile_scene") {
            m_engine->compile_scene();
            m_clear_next_frame = true;
            return ok_response_null();
        }
        if (cmd == "get_viewport_resolution") {
            auto [w, h] = m_engine->resolution();
            return ok_response(json::array({w, h}));
        }
        if (cmd == "set_viewport_resolution") {
            const uint32_t w = req.at("width").get<uint32_t>();
            const uint32_t h = req.at("height").get<uint32_t>();
            m_engine->set_resolution(w, h);
            m_engine->initialize_path_tracer();
            return ok_response_null();
        }
        if (cmd == "get_samples_per_frame") {
            return ok_response(m_engine->samples_per_frame());
        }
        if (cmd == "set_samples_per_frame") {
            m_engine->set_samples_per_frame(req.at("samples").get<uint32_t>());
            return ok_response_null();
        }
        if (cmd == "get_max_bounces") {
            return ok_response(m_engine->max_bounces());
        }
        if (cmd == "set_max_bounces") {
            m_engine->set_max_bounces(req.at("bounces").get<uint32_t>());
            return ok_response_null();
        }

        // ── IO ──
        if (cmd == "save_scene") {
            const auto path = req.at("path").get<std::string>();
            save_scene_to_file(m_engine->scene(), path);
            return ok_response_null();
        }
        if (cmd == "load_scene") {
            const auto path = req.at("path").get<std::string>();
            load_scene_from_file(m_engine->scene(), path);
            return ok_response_null();
        }
        if (cmd == "import_gltf") {
            const auto path = req.at("path").get<std::string>();
            m_engine->load_gltf(path);
            return ok_response_null();
        }
        if (cmd == "export_image") {
            const auto path = req.at("path").get<std::string>();
            const auto format = req.at("format").get<std::string>();
            if (m_last_render_pixels.empty())
                return err_response("No render available");
            if (format != "png" && format != "PNG" && format != "raw")
                return err_response("Unsupported format: " + format);
            std::ofstream out(path, std::ios::binary);
            if (!out) return err_response("Failed to open file: " + path);
            out.write(reinterpret_cast<const char*>(m_last_render_pixels.data()),
                      static_cast<std::streamsize>(m_last_render_pixels.size()));
            return ok_response_null();
        }

        // ── Viewport surface (native overlay) ──
        if (cmd == "set_viewport_rect") {
            if (!m_window) return err_response("No window");
            const int32_t x = req.value("x", 0);
            const int32_t y = req.value("y", 0);
            const uint32_t w = req.value("width", 0u);
            const uint32_t h = req.value("height", 0u);
            m_window->set_viewport_rect(x, y, w, h);
            m_viewport_active = (w > 0 && h > 0);
            const uint32_t pw = m_window->viewport_pixel_width();
            const uint32_t ph = m_window->viewport_pixel_height();
            ensure_viewport_renderer(pw, ph);

            // Push the new aspect ratio to the scene camera so the next
            // render has correct projection. Reset accumulation since the
            // aspect change invalidates the running mean.
            if (ph > 0 && m_engine->scene().hasCamera()) {
                tracey::Camera cam = m_engine->scene().camera();
                const float new_aspect = static_cast<float>(pw) / static_cast<float>(ph);
                if (std::abs(cam.aspectRatio() - new_aspect) > 1e-4f) {
                    cam.setAspectRatio(new_aspect);
                    m_engine->scene().setCamera(cam);
                    m_clear_next_frame = true;
                }
            }
            return ok_response_null();
        }
        if (cmd == "set_viewport_visible") {
            if (!m_window) return err_response("No window");
            const bool vis = req.value("visible", true);
            m_window->set_viewport_visible(vis);
            m_viewport_active = vis;
            return ok_response_null();
        }

        // ── Native dialogs (replace tauri-plugin-dialog) ──
        if (cmd == "open_file_dialog") {
            if (!m_window) return err_response("No window for dialog");
            const auto title = req.value("title", std::string{"Open"});
            std::vector<FileFilter> filters;
            if (req.contains("filters")) {
                for (const auto& f : req["filters"]) {
                    FileFilter ff;
                    ff.description = f.value("description", std::string{});
                    if (f.contains("extensions")) {
                        for (const auto& e : f["extensions"])
                            ff.extensions.push_back(e.get<std::string>());
                    }
                    filters.push_back(std::move(ff));
                }
            }
            const auto path = m_window->open_file_dialog(title.c_str(), filters);
            return ok_response(path.empty() ? json(nullptr) : json(path));
        }
        if (cmd == "save_file_dialog") {
            if (!m_window) return err_response("No window for dialog");
            const auto title = req.value("title", std::string{"Save"});
            const auto default_name = req.value("default_name", std::string{});
            std::vector<FileFilter> filters;
            if (req.contains("filters")) {
                for (const auto& f : req["filters"]) {
                    FileFilter ff;
                    ff.description = f.value("description", std::string{});
                    if (f.contains("extensions")) {
                        for (const auto& e : f["extensions"])
                            ff.extensions.push_back(e.get<std::string>());
                    }
                    filters.push_back(std::move(ff));
                }
            }
            const auto path =
                m_window->save_file_dialog(title.c_str(), default_name.c_str(), filters);
            return ok_response(path.empty() ? json(nullptr) : json(path));
        }
        if (cmd == "open_folder_dialog") {
            if (!m_window) return err_response("No window for dialog");
            const auto title = req.value("title", std::string{"Open Folder"});
            const auto path = m_window->open_folder_dialog(title.c_str());
            return ok_response(path.empty() ? json(nullptr) : json(path));
        }

        return err_response("Unknown command: " + cmd);

    } catch (const json::exception& e) {
        return err_response(std::string{"JSON error: "} + e.what());
    } catch (const std::exception& e) {
        return err_response(e.what());
    } catch (...) {
        return err_response("Unknown error");
    }
}

}  // namespace tracey_editor
