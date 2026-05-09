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
#include "graph/graphs/shader_graph/serialization.hpp"

#include "geometry/geometry_converter.hpp"
#include "sops/serialization.hpp"
#include "sops/sop_graph.hpp"
#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"

#include <glm/gtc/quaternion.hpp>

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
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
    // material_assigned: true if this actor has a non-empty graph attached.
    // The frontend doesn't need the full JSON for the actor inspector -- a
    // boolean is enough to indicate "uses a library graph" vs "passthrough".
    return {
        {"id", a.getUid()},
        {"name", a.name()},
        {"transform", transform_to_json(a.transform())},
        {"children", std::move(children)},
        {"material_assigned", !a.materialGraphJson().empty()},
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
    // Field names must match what scene_compiler reads -- it looks for "albedo"
    // (via MaterialInstance::albedo()), not "baseColor".
    material.setAlbedo(tracey::Vec3(0.8f, 0.8f, 0.8f));
    material.setMetallic(0.0f);
    material.setRoughness(0.5f);

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

// ─── Material library: persistent per-user store of saved graphs ────────────
// Names keep the file format simple (one .json per graph) so the directory
// stays git-diff-friendly. We sanitize names to keep paths bounded.

std::filesystem::path material_library_dir() {
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) /
               "Library/Application Support/Tracey/MaterialLibrary";
    }
#elif defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "Tracey" / "MaterialLibrary";
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path(xdg) / "tracey" / "material_library";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local/share/tracey/material_library";
    }
#endif
    return std::filesystem::current_path() / "material_library";
}

bool is_safe_library_name(const std::string& name) {
    if (name.empty() || name.size() > 96) return false;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

EditorServer::EditorServer(std::unique_ptr<RenderEngine> engine, EditorWindow* window)
    : m_engine(std::move(engine)), m_window(window) {
    // SOP framework setup. Register the v1 built-in nodes once per process
    // before the first set_sop_graph / catalog query lands. Idempotent-ish:
    // calling registerBuiltinSops() twice would duplicate entries, so it
    // only runs the first time an EditorServer is constructed.
    static bool s_sopsRegistered = false;
    if (!s_sopsRegistered) {
        tracey::sops::registerBuiltinSops();
        s_sopsRegistered = true;
    }
    m_sop_graph = std::make_unique<tracey::sops::SopGraph>(0);
}

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

// Cook the SOP graph and rebuild the live scene from the result. Must be
// called with m_mutex held; runs on the main thread (touches Vulkan via the
// final compile_scene() call).
//
// Strategy: replace all existing actors + scene objects with whatever the
// cook emits. We don't try to incrementally update — even moderately complex
// graphs can change topology arbitrarily, and the path tracer's recompile
// already costs more than scene rebuild. Material assignments survive across
// cooks because they're stored on the ObjectOutput SOP node's
// `material_library_name` parameter and re-resolved here.
void EditorServer::cook_and_apply() {
    if (!m_sop_graph || !m_engine) return;

    tracey::sops::CookDiagnostic diag;
    auto emitted = m_sop_graph->cook(&diag);
    if (!diag.ok) {
        std::fprintf(stderr, "[sop] cook failed: %s (node uid=%zu)\n",
                     diag.message.c_str(), diag.nodeUid);
        return;
    }

    // Clear and rebuild the scene's actors + named SceneObjects. The camera
    // state is independent of the SOP graph (driven by user fly-through) so
    // we preserve it across cooks; otherwise every parameter tweak would
    // snap the viewport back to the default angle.
    auto& scene = m_engine->scene();
    std::optional<tracey::Camera> savedCamera;
    if (scene.hasCamera()) savedCamera = scene.camera();
    scene.clear();
    if (savedCamera) scene.setCamera(*savedCamera);
    m_object_output_to_actor.clear();

    for (const auto& ea : emitted) {
        // Each emitted actor's geometry becomes a uniquely-named SceneObject.
        const std::string objectName = ea.name.empty() ? "actor" : ea.name;
        scene.addObject(objectName, tracey::GeometryConverter::toSceneObject(ea.geometry, objectName));

        auto* actor = scene.createActor();
        actor->setName(objectName);

        tracey::Transform xform;
        xform.setPosition(ea.translate);
        // ea.rotation is a quaternion (wxyz); v1 cook emits identity but we
        // honour it if the codegen path or a future TransformParam fills it in.
        xform.setRotation(tracey::Quaternion(ea.rotation.x, ea.rotation.y,
                                             ea.rotation.z, ea.rotation.w));
        xform.setScale(ea.scale);
        actor->setTransform(xform);

        // If the user assigned a material library to this output, attach the
        // graph JSON now so the next compile_scene picks it up.
        if (!ea.materialLibraryName.empty() &&
            is_safe_library_name(ea.materialLibraryName)) {
            std::ifstream in(material_library_dir() / (ea.materialLibraryName + ".json"));
            if (in) {
                std::stringstream ss;
                ss << in.rdbuf();
                actor->setMaterialGraphJson(ss.str());
            }
        }

        // Default material instance referencing the SceneObject by name.
        tracey::MaterialInstance mat("pbr");
        mat.setAlbedo(tracey::Vec3(0.8f));
        mat.setMetallic(0.0f);
        mat.setRoughness(0.5f);
        actor->addInstance(tracey::SceneInstance(objectName, mat));

        // Stable actor↔SOP mapping via the uid threaded through EmittedActor.
        // The actor we just appended is at the back of scene.actors().
        if (ea.sourceNodeUid != 0 && !scene.actors().empty()) {
            m_object_output_to_actor[ea.sourceNodeUid] =
                scene.actors().back()->getUid();
        }
    }

    // Recompile so the path tracer picks up the new BLAS/TLAS + material
    // programs, and reset accumulation.
    if (m_engine->path_tracer_ready()) {
        m_engine->compile_scene();
        m_clear_next_frame = true;
    }

    if (m_broadcast) {
        m_broadcast(R"({"event":"scene_changed"})");
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
            const auto xform = transform_from_json(req.at("transform"));
            a->setTransform(xform);

            // If this actor was emitted by a SOP graph object_output node,
            // write the transform back into that node's parameters so the
            // edit survives the next cook (instead of getting clobbered).
            // For v1 we only persist translate + scale; rotation passes
            // through to the path tracer immediately but isn't round-tripped
            // through the SOP node yet (object_output's rotation params are
            // euler-deg and the wire here is a quaternion — quat→euler with
            // gimbal handling is a deferral).
            //
            // Note: we don't trigger a re-cook here; the actor's transform is
            // already what the user wants and a cook would replace it with
            // the same value. The frontend's local SOP store IS now stale
            // though, so we broadcast `sop_graph_changed` to nudge it to
            // reload (sops.ts listens; race window with mid-edit pushes is
            // small and accepted for v1).
            bool sopMutated = false;
            if (m_sop_graph) {
                for (const auto& [outputUid, actorUid] : m_object_output_to_actor) {
                    if (actorUid != id) continue;
                    auto* node = m_sop_graph->findNode(outputUid);
                    if (!node) break;
                    node->setParamVec3("translate", xform.position());
                    node->setParamVec3("scale", xform.scale());
                    sopMutated = true;
                    break;
                }
            }
            m_clear_next_frame = true;
            if (sopMutated && m_broadcast) {
                m_broadcast(R"({"event":"sop_graph_changed"})");
            }
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

        // ── Material graphs ──
        if (cmd == "get_material_graph") {
            return ok_response(m_engine->get_material_graph_json());
        }
        if (cmd == "set_material_graph") {
            const auto graph_json = req.at("graph").get<std::string>();
            m_engine->set_material_graph_json(graph_json);
            m_clear_next_frame = true;  // accumulator invalid after material change
            return ok_response_null();
        }
        if (cmd == "set_material_parameter") {
            const uint32_t program_id = req.at("program_id").get<uint32_t>();
            const uint32_t param_idx = req.at("param_idx").get<uint32_t>();
            const auto& v = req.at("value");
            m_engine->set_material_parameter(
                program_id, param_idx,
                v[0].get<float>(), v[1].get<float>(),
                v[2].get<float>(), v[3].get<float>());
            m_clear_next_frame = true;
            return ok_response_null();
        }

        // ── Material library (per-user persistent graphs) ──
        if (cmd == "list_material_library") {
            std::filesystem::path dir = material_library_dir();
            json arr = json::array();
            std::error_code ec;
            if (std::filesystem::exists(dir, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (!entry.is_regular_file()) continue;
                    if (entry.path().extension() != ".json") continue;
                    arr.push_back(entry.path().stem().string());
                }
            }
            std::sort(arr.begin(), arr.end());
            return ok_response(arr);
        }
        if (cmd == "save_material_graph_as") {
            const auto name = req.at("name").get<std::string>();
            const auto graph_json = req.at("graph").get<std::string>();
            if (!is_safe_library_name(name)) return err_response("invalid library name");
            // Pretty-print so the file is git-diff-friendly.
            auto graph = tracey::deserializeShaderGraph(graph_json);
            if (!graph) return err_response("could not parse graph json");
            const std::string pretty = tracey::serializeShaderGraphPretty(*graph);

            std::filesystem::path dir = material_library_dir();
            std::filesystem::create_directories(dir);
            std::ofstream out(dir / (name + ".json"));
            if (!out) return err_response("could not open file for writing");
            out << pretty;
            return ok_response_null();
        }
        if (cmd == "load_material_graph_from_library") {
            const auto name = req.at("name").get<std::string>();
            if (!is_safe_library_name(name)) return err_response("invalid library name");
            std::filesystem::path file = material_library_dir() / (name + ".json");
            std::ifstream in(file);
            if (!in) return err_response("graph not found in library");
            std::stringstream ss;
            ss << in.rdbuf();
            return ok_response(ss.str());
        }
        if (cmd == "delete_material_graph_from_library") {
            const auto name = req.at("name").get<std::string>();
            if (!is_safe_library_name(name)) return err_response("invalid library name");
            std::error_code ec;
            std::filesystem::remove(material_library_dir() / (name + ".json"), ec);
            return ok_response_null();
        }

        // ── Per-actor material assignment ──
        // Resolve a library entry to a graph JSON server-side, attach it to
        // the actor, recompile the scene so the new MaterialProgramBuffer +
        // instanceProgramIndex SSBO take effect, and invalidate accumulation.
        // An empty `library_name` clears the assignment back to passthrough.
        if (cmd == "set_actor_material") {
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            const auto name = req.value("library_name", std::string{});

            auto* actor = m_engine->scene().getActor(id);
            if (!actor) return err_response("actor not found");

            std::string graph_json;
            if (!name.empty()) {
                if (!is_safe_library_name(name)) return err_response("invalid library name");
                std::ifstream in(material_library_dir() / (name + ".json"));
                if (!in) return err_response("library entry not found");
                std::stringstream ss;
                ss << in.rdbuf();
                graph_json = ss.str();
            }
            actor->setMaterialGraphJson(graph_json);

            if (m_engine->compiled_scene_ready()) {
                m_engine->compile_scene();
                m_clear_next_frame = true;
            }
            return ok_response_null();
        }

        // ── SOP graph (scene-level Houdini-style /obj network) ──
        // Mirrors the material-graph commands: catalog query + get/set the
        // whole graph as JSON. Frontend mutates locally and debounce-pushes
        // the full graph back; no fan-out of fine-grained sop_create_node /
        // sop_connect commands.
        if (cmd == "list_sop_node_catalog") {
            json arr = json::array();
            for (const auto& e : tracey::sops::SopRegistry::instance().catalog()) {
                json inputs = json::array();
                for (const auto& p : e.inputs) inputs.push_back({{"name", p.name}});
                json outputs = json::array();
                for (const auto& p : e.outputs) outputs.push_back({{"name", p.name}});
                json params = json::array();
                for (const auto& p : e.params) {
                    params.push_back({
                        {"name",     p.name},
                        {"type",     tracey::sops::paramTypeName(p.type)},
                        {"default",  p.defaultRepr},
                    });
                }
                arr.push_back({
                    {"kind",     e.kind},
                    {"label",    e.label},
                    {"category", e.category},
                    {"inputs",   inputs},
                    {"outputs",  outputs},
                    {"params",   params},
                });
            }
            return ok_response(arr);
        }
        if (cmd == "get_sop_graph") {
            if (!m_sop_graph) return ok_response("");
            return ok_response(tracey::sops::serializeSopGraph(*m_sop_graph));
        }
        if (cmd == "set_sop_graph") {
            const auto graph_json = req.at("graph").get<std::string>();
            std::unique_ptr<tracey::sops::SopGraph> parsed;
            try {
                parsed = tracey::sops::deserializeSopGraph(graph_json);
            } catch (const std::exception& e) {
                return err_response(std::string("sop graph parse error: ") + e.what());
            }
            m_sop_graph = std::move(parsed);
            cook_and_apply();
            return ok_response_null();
        }

        // ── IO ──
        // v2 schema:
        //   { "version": 2, "scene": <scene_state v1 payload>, "sop_graph": "<serialized SopGraph>" }
        // v1 (legacy) files load best-effort: actors + camera populate the
        // scene, but no SOP graph is recovered (a v1 file predates the SOP
        // backend by definition).
        if (cmd == "save_scene") {
            const auto path = req.at("path").get<std::string>();
            // Write a temp v1-payload to disk, read it back into json, embed.
            const std::filesystem::path tmp =
                std::filesystem::path(path).replace_extension(".__tmp__.json");
            save_scene_to_file(m_engine->scene(), tmp.string());
            json sceneJson;
            {
                std::ifstream in(tmp);
                in >> sceneJson;
            }
            std::error_code ec;
            std::filesystem::remove(tmp, ec);

            json root;
            root["version"] = 2;
            root["scene"] = std::move(sceneJson);
            root["sop_graph"] = m_sop_graph
                ? tracey::sops::serializeSopGraphPretty(*m_sop_graph)
                : std::string{};

            std::ofstream out(path);
            if (!out) return err_response("could not open file for writing: " + path);
            out << root.dump(2);
            return ok_response_null();
        }
        if (cmd == "load_scene") {
            const auto path = req.at("path").get<std::string>();
            std::ifstream in(path);
            if (!in) return err_response("could not open file for reading: " + path);
            json root;
            try { in >> root; }
            catch (const std::exception& e) {
                return err_response(std::string("scene parse error: ") + e.what());
            }

            const int version = root.value("version", 1);
            if (version == 2) {
                // Pull the inner "scene" payload out to a temp file the v1
                // loader understands, then load.
                const std::filesystem::path tmp =
                    std::filesystem::path(path).replace_extension(".__tmp__.json");
                {
                    std::ofstream tout(tmp);
                    tout << root.value("scene", json::object()).dump(2);
                }
                load_scene_from_file(m_engine->scene(), tmp.string());
                std::error_code ec;
                std::filesystem::remove(tmp, ec);

                const auto sopJson = root.value("sop_graph", std::string{});
                if (!sopJson.empty()) {
                    try {
                        m_sop_graph = tracey::sops::deserializeSopGraph(sopJson);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "[sop] load failed: %s\n", e.what());
                        m_sop_graph = std::make_unique<tracey::sops::SopGraph>(0);
                    }
                    cook_and_apply();
                }
            } else {
                // Legacy v1 file: scene fields are at the root.
                load_scene_from_file(m_engine->scene(), path);
                m_sop_graph = std::make_unique<tracey::sops::SopGraph>(0);
            }
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
