#include "editor_server.hpp"

#include "platform/platform.hpp"
#include "scene_state.hpp"
#include "video_exporter.hpp"

#include "core/parallel.hpp"
#include "scene/actor.hpp"
#include "scene/camera.hpp"
#include "scene/gltf_loader.hpp"
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
#include "sops/nodes/attribute_vop_sop.hpp"
#include "sops/nodes/dop_import_sop.hpp"
#include "vops/vop_graph.hpp"
#include "vops/vop_registry.hpp"
#include "vops/serialization.hpp"
#include "vops/register_builtins.hpp"
#include "dops/dop_graph.hpp"
#include "dops/dop_node.hpp"
#include "dops/dop_registry.hpp"
#include "dops/serialization.hpp"
#include "dops/register_builtins.hpp"
#include "dops/nodes/pop_force.hpp"

#include "vops/codegen/compute_dispatch.hpp"
#include "sops/codegen/copy_to_points_compute.hpp"
#include "sops/codegen/transform_compute.hpp"
#include "sops/codegen/merge_compute.hpp"
#include "geometry/attribute_allocator.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>  // glm::rotation for from→to vector quat
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt for default camera pose

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
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

json actor_to_json(const tracey::Actor& a, size_t sourceSopNodeUid) {
    json children = json::array();
    for (size_t uid : a.children()) children.push_back(uid);
    // material_assigned: true if this actor has a non-empty graph attached.
    // The frontend doesn't need the full JSON for the actor inspector -- a
    // boolean is enough to indicate "uses a library graph" vs "passthrough".
    json out = {
        {"id", a.getUid()},
        {"name", a.name()},
        {"transform", transform_to_json(a.transform())},
        {"children", std::move(children)},
        {"material_assigned", !a.materialGraphJson().empty()},
    };
    // sop_node_uid: the object_output node that emitted this actor. Lets the
    // frontend target keyframe edits at the right SOP parameter without an
    // extra round-trip. Null for actors not produced by the cook.
    out["sop_node_uid"] = sourceSopNodeUid != 0
        ? json(static_cast<uint64_t>(sourceSopNodeUid))
        : json(nullptr);
    // Display flag for the eye-icon toggle in the hierarchy.
    out["visible"] = a.visible();
    // Light component (Houdini-style /obj light). When present, the hierarchy
    // panel swaps the actor's icon and the inspector exposes light params.
    if (a.hasLight()) {
        const auto* lt = a.light();
        // Forward every field on Light so the inspector can render the
        // type-conditional rows without a second IPC. The Dome gradient
        // fields stay populated even for non-Dome lights — their default
        // values are harmless, and shipping them now means switching the
        // light type via the dropdown doesn't lose any prior tweaks.
        out["light"] = {
            {"type",          static_cast<int>(lt->type)},
            {"color",         {{"x", lt->color.x},        {"y", lt->color.y},        {"z", lt->color.z}}},
            {"intensity",     lt->intensity},
            {"sky_color",     {{"x", lt->skyColor.x},     {"y", lt->skyColor.y},     {"z", lt->skyColor.z}}},
            {"horizon_color", {{"x", lt->horizonColor.x}, {"y", lt->horizonColor.y}, {"z", lt->horizonColor.z}}},
            {"ground_color",  {{"x", lt->groundColor.x},  {"y", lt->groundColor.y},  {"z", lt->groundColor.z}}},
            {"hdri_path",     lt->hdriPath},
            {"size",          {{"x", lt->size.x},         {"y", lt->size.y}}},
            {"radius",        lt->radius},
        };
    } else {
        out["light"] = json(nullptr);
    }
    // parent_id: uid of the parent actor in the live scene tree, or null
    // when this actor is at the root. Subnets create parent actors via
    // EmittedActor markers, so children of a subnet point at the subnet
    // actor here; flat object_output actors have no parent.
    out["parent_id"] = a.hasParent()
        ? json(static_cast<uint64_t>(a.parent()))
        : json(nullptr);
    return out;
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

// User-wide material library — palette shared across projects. Matches
// the legacy `material_library_dir` location so previously-saved
// materials remain reachable as global entries.
std::filesystem::path global_material_dir() {
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

}  // namespace

std::filesystem::path EditorServer::project_material_dir() const {
    if (m_project_dir.empty()) return {};
    return m_project_dir / "materials";
}

std::filesystem::path EditorServer::resolve_material_path(const std::string &name) const {
    // Project-local materials shadow global ones with the same name —
    // the rationale being a project file declares its own copy of a
    // shared material when it wants to deviate, and that override
    // should win.
    std::error_code ec;
    if (!m_project_dir.empty()) {
        std::filesystem::path local = project_material_dir() / (name + ".json");
        if (std::filesystem::exists(local, ec)) return local;
    }
    std::filesystem::path global = global_material_dir() / (name + ".json");
    if (std::filesystem::exists(global, ec)) return global;
    return {};
}

namespace {

bool is_safe_library_name(const std::string& name) {
    if (name.empty() || name.size() > 96) return false;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ';
        if (!ok) return false;
    }
    return true;
}

// Recursive lookup that descends into subnet inner graphs. Node uids are
// globally unique across nesting (SopGraph::nextUid forwards to the root
// allocator), so the keyframe IPC and the animation override path can
// identify nodes by uid alone — no path qualifier needed on the wire.
tracey::sops::SopNode* findNodeRecursive(tracey::sops::SopGraph* g, size_t uid) {
    if (!g) return nullptr;
    if (auto* hit = g->findNode(uid)) return hit;
    for (const auto& n : g->nodes()) {
        if (auto* sn = dynamic_cast<tracey::sops::SopNode*>(n.get())) {
            if (auto* inner = sn->innerGraph()) {
                if (auto* hit = findNodeRecursive(inner, uid)) return hit;
            }
        }
    }
    return nullptr;
}

// DOP-side companion. Flat lookup in v1 — DopGraph has no subnet
// equivalent. Returned by the VOP IPC handlers as the host-fallback path
// when the uid doesn't resolve to a SOP node (pop_force is the only
// DOP-hosted VOP subnet right now).
tracey::dops::DopNode* findDopNode(tracey::dops::DopGraph* g, size_t uid) {
    if (!g) return nullptr;
    return g->findNode(uid);
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
        tracey::vops::registerBuiltinVops();
        tracey::dops::registerBuiltinDops();
        s_sopsRegistered = true;
    }
    m_sop_graph = std::make_unique<tracey::sops::SopGraph>(0);
    m_dop_graph = std::make_unique<tracey::dops::DopGraph>(0);

    // Register the engine's device with AttributeAllocator so any
    // Attribute<T>::buffer() call across the codebase can lazily
    // allocate its GPU storage against the right Vulkan context.
    // This is the foundation for the GPU-resident Geometry refactor
    // (Phase A); the dispatcher + future SOP kernels read it the
    // same way.
    if (m_engine) {
        tracey::AttributeAllocator::setDevice(&m_engine->device());
        // Match the engine's BVH-build flag to the live-viewport default
        // (PT inset preview off → no BVHs on the first compile). The
        // set_pt_preview command flips both flags together once the user
        // explicitly enables the preview.
        m_engine->set_build_acceleration_structures(m_pt_preview_enabled);
    }

    // Wire up the GPU VOP dispatcher against the engine's compute
    // device. Fail-safe: any throw here just leaves attribute_vop /
    // pop_force on the CPU code path — we don't want a missing
    // compute backend to break the editor.
    if (m_engine) {
        try {
            m_vop_dispatcher = std::make_unique<tracey::vops::codegen::VopComputeDispatcher>(
                &m_engine->device());
            tracey::vops::codegen::VopComputeDispatcher::setGlobal(m_vop_dispatcher.get());
            std::fprintf(stderr, "[vop:compute] GPU dispatcher ready\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[vop:compute] GPU dispatcher unavailable, "
                                 "falling back to CPU: %s\n", e.what());
        }
        // copy_to_points has its own dispatcher (fixed kernel, feature-
        // flagged variants). Same fail-safe contract: a missing GPU
        // dispatcher leaves the SOP on the CPU path.
        try {
            m_ctp_dispatcher = std::make_unique<tracey::sops::codegen::CopyToPointsCompute>(
                &m_engine->device());
            tracey::sops::codegen::CopyToPointsCompute::setGlobal(m_ctp_dispatcher.get());
            std::fprintf(stderr, "[ctp:compute] GPU dispatcher ready\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ctp:compute] GPU dispatcher unavailable, "
                                 "falling back to CPU: %s\n", e.what());
        }
        // transform SOP has its own dispatcher (in-place SRT compute
        // for Vec3 attributes). Same fail-safe contract.
        try {
            m_xform_dispatcher = std::make_unique<tracey::sops::codegen::TransformCompute>(
                &m_engine->device());
            tracey::sops::codegen::TransformCompute::setGlobal(m_xform_dispatcher.get());
            std::fprintf(stderr, "[xform:compute] GPU dispatcher ready\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[xform:compute] GPU dispatcher unavailable, "
                                 "falling back to CPU: %s\n", e.what());
        }
        // merge SOP — vkCmdCopyBuffer-based concat. No shader, just
        // owns the Vulkan device handle. Same fail-safe contract.
        try {
            m_merge_dispatcher = std::make_unique<tracey::sops::codegen::MergeCompute>(
                &m_engine->device());
            tracey::sops::codegen::MergeCompute::setGlobal(m_merge_dispatcher.get());
            std::fprintf(stderr, "[merge:compute] GPU dispatcher ready\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[merge:compute] GPU dispatcher unavailable, "
                                 "falling back to CPU: %s\n", e.what());
        }
    }

    m_cook_thread = std::thread([this] { cook_worker_loop(); });

    // Render worker. Started AFTER m_engine is fully initialised so the
    // first dispatch isn't racing the rasterizer/path-tracer ctors.
    m_render_thread = std::thread([this] { render_thread_main(); });

    // Default-Dome spawn. A fresh editor session opens with no scene
    // file; without this the viewport stays unlit until the user adds a
    // light (the shader has an unlit-albedo fallback, but Blender/Unity
    // convention is to have an env light from the start). Skipped if
    // the scene already contains any actor — load_scene runs before
    // this on app-launch-with-project, and a project that intentionally
    // ships no Dome should stay that way.
    if (m_engine && m_engine->scene().actors().empty()) {
        auto* actor = m_engine->scene().createActor();
        actor->setName("Dome");
        tracey::Light light;
        light.type = tracey::LightType::Dome;
        actor->setLight(light);
    }
}

EditorServer::~EditorServer() {
    // Tell the export worker to bail; join before tearing down the engine
    // since the worker writes to the path tracer.
    m_export_cancel.store(true);
    if (m_export_thread.joinable()) m_export_thread.join();

    {
        std::lock_guard<std::mutex> lk(m_cook_request_mutex);
        m_cook_shutdown = true;
    }
    m_cook_request_cv.notify_one();
    if (m_cook_thread.joinable()) m_cook_thread.join();

    // Render worker also has to stop BEFORE the engine tears down — it
    // dereferences m_engine->rasterizer/path_tracer on every iteration.
    stop_render_thread();

    // Clear the global dispatcher BEFORE the engine (and its device)
    // tear down, so any late cook that races shutdown sees nullptr
    // and falls back to CPU rather than dereferencing a destroyed
    // VkDevice.
    tracey::vops::codegen::VopComputeDispatcher::setGlobal(nullptr);
    m_vop_dispatcher.reset();
    tracey::sops::codegen::CopyToPointsCompute::setGlobal(nullptr);
    m_ctp_dispatcher.reset();
    tracey::sops::codegen::TransformCompute::setGlobal(nullptr);
    m_xform_dispatcher.reset();
    tracey::sops::codegen::MergeCompute::setGlobal(nullptr);
    m_merge_dispatcher.reset();
    // And the attribute allocator — once the device goes, any
    // pending Attribute<T>::buffer() / bufferConst() call from a
    // late cook must see "no device available" and skip the GPU
    // path instead of holding a dangling pointer.
    tracey::AttributeAllocator::setDevice(nullptr);

    // Tear the viewport (and its VkSurface) down explicitly, while the engine
    // — and therefore the VkInstance the surface was created from, and the
    // CAMetalLayer it wraps — are still guaranteed alive. Member-destruction
    // order would do the same thing implicitly, but stating it here protects
    // against future header re-ordering.
    m_viewport.reset();
}

void EditorServer::set_broadcast_callback(BroadcastCallback cb) {
    m_broadcast = std::move(cb);
}

void EditorServer::broadcast(const std::string& message) {
    if (m_broadcast) m_broadcast(message);
}

// Top-right path-tracer PiP geometry. ~25% of viewport width, square-pixel
// matched to the viewport's aspect so projection is the same in both views.
// Returns {x, y, w, h} in swapchain pixels.
struct InsetRect { int32_t x, y; uint32_t w, h; };
static InsetRect compute_inset_rect(uint32_t vp_w, uint32_t vp_h) {
    constexpr uint32_t MIN_W = 64;
    constexpr float FRACTION = 0.25f;
    constexpr int32_t MARGIN = 8;
    uint32_t w = static_cast<uint32_t>(std::round(vp_w * FRACTION));
    if (w < MIN_W) w = std::min<uint32_t>(MIN_W, vp_w);
    uint32_t h = static_cast<uint32_t>(std::round(static_cast<uint64_t>(w) * vp_h / std::max<uint32_t>(vp_w, 1)));
    if (h == 0) h = 1;
    int32_t x = static_cast<int32_t>(vp_w) - static_cast<int32_t>(w) - MARGIN;
    int32_t y = MARGIN;
    if (x < 0) x = 0;
    return {x, y, w, h};
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
            // Right-size the rasterizer (full viewport) and PT (inset).
            const InsetRect r = compute_inset_rect(pixel_w, pixel_h);
            m_engine->set_resolutions(pixel_w, pixel_h, r.w, r.h);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[viewport] failed to create renderer: %s\n", e.what());
            m_viewport.reset();
        }
        return;
    }

    if (pixel_w != m_viewport_pixel_w || pixel_h != m_viewport_pixel_h) {
        // Resize the swapchain only (cheap — a flag flip; the actual recreate
        // happens lazily on the next present). The renderer pipelines stay
        // at their current size until set_viewport_resolution arrives, which
        // the frontend debounces (250ms). Without this split a splitter drag
        // would tear down + rebuild the path tracer and rasterizer per pixel.
        m_viewport->resize(pixel_w, pixel_h);
        m_viewport_pixel_w = pixel_w;
        m_viewport_pixel_h = pixel_h;
        m_clear_next_frame = true;
    }
}

// Pick the right path-tracer render size for the current (fullscreen,
// override, viewport) tuple and push it to the engine. Called whenever
// any of those inputs changes (set_pt_fullscreen, set_pt_render_resolution,
// or viewport resize via ensure_viewport_renderer). Idempotent — silently
// no-ops when the viewport hasn't been initialised yet.
void EditorServer::apply_pt_resolution() {
    if (!m_engine || m_viewport_pixel_w == 0 || m_viewport_pixel_h == 0) return;

    uint32_t pt_w, pt_h;
    if (m_pt_fullscreen) {
        // Render workspace. Honour the override when set; otherwise
        // match the viewport so there's no upscale.
        if (m_pt_render_w > 0 && m_pt_render_h > 0) {
            pt_w = m_pt_render_w;
            pt_h = m_pt_render_h;
        } else {
            pt_w = m_viewport_pixel_w;
            pt_h = m_viewport_pixel_h;
        }
    } else {
        // PiP composite. Resolution always tracks the inset rect; the
        // override is ignored because at PiP size it'd just be wasteful.
        const InsetRect r = compute_inset_rect(m_viewport_pixel_w, m_viewport_pixel_h);
        pt_w = r.w;
        pt_h = r.h;
    }
    m_engine->set_resolutions(m_viewport_pixel_w, m_viewport_pixel_h, pt_w, pt_h);
    // Resolution changed → accumulator buffer was reallocated; clear so
    // we don't show one frame of garbage on transition.
    m_clear_next_frame = true;
}

// Cook the SOP graph and rebuild the live scene from the result. Must be
// called with m_mutex held; runs on the main thread (touches Vulkan via the
// final compile_scene() call).
//
// Synchronous path used for one-shot operations (load_scene). The live
// editing path goes through post_cook_request → cook_worker_loop →
// drain_cook_result + apply_emitted instead.
void EditorServer::cook_and_apply() {
    if (!m_sop_graph || !m_engine) return;

    // Same cook_status broadcast as the async path so the frontend's loading
    // status fires on synchronous load_scene cooks too.
    if (m_broadcast) m_broadcast(R"({"event":"cook_status","busy":true})");

    // Stamp the current-frame sim geometry into every dop_import SOP
    // before cooking. The async path uses CookRequest::dop_stamps for
    // this; the synchronous path (video export, load_scene) doesn't go
    // through that worker, so without this the dop_import nodes cook
    // with their default-empty stamp and the rendered actors come back
    // with zero vertices — i.e. the exported video frame is solid black.
    if (m_dop_graph && m_has_dop_imports) {
        auto stamps = collect_dop_stamps(m_timeline.current_time);
        for (auto& [uid, geo] : stamps) {
            if (auto* node = findNodeRecursive(m_sop_graph.get(), uid)) {
                tracey::sops::setDopImportGeometry(node, std::move(geo));
            }
        }
    }

    tracey::sops::CookDiagnostic diag;
    // Same Houdini-style per-node cook cache as the worker path, just a
    // separate instance — this one's owned by the main thread under
    // m_mutex. Reuses cached Geometry for any node whose
    // (kind, params, upstream cookIds) fingerprint is unchanged.
    m_main_cook_cache.markAllUntouched();
    auto emitted = m_sop_graph->cook(&diag, m_timeline.current_time,
                                     &m_main_cook_cache);
    m_main_cook_cache.evictUntouched();
    if (!diag.ok) {
        std::fprintf(stderr, "[sop] cook failed: %s (node uid=%zu)\n",
                     diag.message.c_str(), diag.nodeUid);
        if (m_broadcast) m_broadcast(R"({"event":"cook_status","busy":false})");
        return;
    }
    apply_emitted(std::move(emitted));
    if (m_broadcast) m_broadcast(R"({"event":"cook_status","busy":false})");
}

// Replace the live scene's actors + named SceneObjects with the emitted set.
// We don't try to incrementally update — even moderately complex graphs can
// change topology arbitrarily, and the path tracer's recompile already costs
// more than scene rebuild. Material assignments survive across cooks because
// they're stored on the ObjectOutput SOP node's `material_library_name`
// parameter and re-resolved here.
//
// Must be called with m_mutex held; main-thread only (Vulkan recompile).
// FNV-1a primitives used by the per-actor + global signatures below.
namespace {
constexpr uint64_t kSigOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kSigPrime  = 0x00000100000001b3ULL;

// Compose a (sourceNodeUid, instanceIndex) into a 64-bit key for the per-
// actor tracking maps. Layout: low 24 bits = instanceIndex (16M instances
// per SOP, comfortably more than any reasonable scatter/instance graph),
// high 40 bits = sourceNodeUid. The high bits stay 40-wide so editor uid
// counters can grow well past 2^32 before any collision risk.
inline uint64_t make_actor_key(size_t uid, uint32_t inst) {
    return (static_cast<uint64_t>(uid) << 24) |
           (static_cast<uint64_t>(inst) & 0xFFFFFFULL);
}
inline uint64_t make_actor_key(const tracey::sops::EmittedActor &a) {
    return make_actor_key(a.sourceNodeUid, a.instanceIndex);
}
inline void sig_mix(uint64_t &h, const void *p, size_t n) {
    // 8-byte bulk path for the per-instance hash walks (transform_sig
    // over 40K particles is a ~1.6 MB sweep — the old per-byte FNV ran
    // at ~600 MB/s, dominating apply_emitted's rebuild bucket). The
    // bulk path runs at L1 bandwidth (~10 GB/s) and the per-byte
    // tail handles non-multiple-of-8 buffers (strings, mat3, etc.)
    // so existing callers stay correct.
    const auto *b = static_cast<const unsigned char *>(p);
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, b, 8);  // memcpy is the canonical unaligned-load idiom
        h ^= v;
        h *= kSigPrime;
        h ^= h >> 31;  // avalanche so byte-shifts within a 64-bit chunk diverge
        b += 8;
        n -= 8;
    }
    while (n > 0) {
        h ^= *b++;
        h *= kSigPrime;
        --n;
    }
}
inline void sig_mix_str(uint64_t &h, const std::string &s) {
    sig_mix(h, s.data(), s.size());
    const uint64_t n = s.size();
    sig_mix(h, &n, sizeof(n));
}

// 64-bit fingerprint of a Geometry payload — captures everything
// `GeometryConverter::toSceneObject` reads (positions, vertex→point
// topology, primitive list, per-point / per-vertex N/uv/Cd). Two
// Geometry values producing identical SceneObjects share this hash, so
// apply_emitted can dedupe them onto one shared SceneObject + BLAS.
//
// Stable within one process run; not for on-disk comparison.
uint64_t geometry_dedup_hash(const tracey::Geometry &g);

// Per-pointer hash cache. Cleared at the start of every apply_emitted /
// emitted_signature pass so two Geometries that happen to reuse the same
// heap address across cooks can't return stale entries — within a single
// pass, the emitted vector holds all the shared_ptrs alive, so pointer
// identity is safe. The whole point of this cache is the instance SOP:
// 120 actors all referencing the same stamp pay the hashing cost once
// instead of 120 times.
std::unordered_map<const tracey::Geometry*, uint64_t> &geometryHashCache() {
    thread_local std::unordered_map<const tracey::Geometry*, uint64_t> c;
    return c;
}
void resetGeometryHashCache() { geometryHashCache().clear(); }

// shared_ptr overload — EmittedActor::geometry is shared so apply_emitted
// can run N instance actors through one alloc. Null (lights, subnet
// markers) → zero, the empty-geometry digest.
uint64_t geometry_dedup_hash(const std::shared_ptr<const tracey::Geometry> &g) {
    if (!g) return 0;
    auto &cache = geometryHashCache();
    const tracey::Geometry *raw = g.get();
    if (auto it = cache.find(raw); it != cache.end()) return it->second;
    const uint64_t h = geometry_dedup_hash(*g);
    cache[raw] = h;
    return h;
}

uint64_t geometry_dedup_hash(const tracey::Geometry &g) {
    uint64_t h = kSigOffset;

    auto mix_vec3_attr = [&](const tracey::Attribute<tracey::Vec3> *a, char tag) {
        if (!a) return;
        sig_mix(h, &tag, 1);
        const uint64_t n = a->data().size();
        sig_mix(h, &n, sizeof(n));
        if (n > 0) sig_mix(h, a->data().data(), n * sizeof(tracey::Vec3));
    };
    auto mix_vec2_attr = [&](const tracey::Attribute<tracey::Vec2> *a, char tag) {
        if (!a) return;
        sig_mix(h, &tag, 1);
        const uint64_t n = a->data().size();
        sig_mix(h, &n, sizeof(n));
        if (n > 0) sig_mix(h, a->data().data(), n * sizeof(tracey::Vec2));
    };

    // Positions — the BLAS-affecting field. Always present after
    // construction; if for some reason absent, the SceneObject would be
    // empty anyway and won't reach the rasterizer.
    if (const auto *P = g.points().get<tracey::Vec3>("P"))
    {
        const uint64_t n = P->data().size();
        sig_mix(h, &n, sizeof(n));
        if (n > 0) sig_mix(h, P->data().data(), n * sizeof(tracey::Vec3));
    }
    // Vertex→point topology + primitive list — same mesh data laid out
    // differently produces different SceneObject corner streams.
    const auto &v2p = g.vertexToPoint();
    const uint64_t vn = v2p.size();
    sig_mix(h, &vn, sizeof(vn));
    if (vn > 0) sig_mix(h, v2p.data(), vn * sizeof(uint32_t));
    const auto &prims = g.primitivesList();
    const uint64_t pn = prims.size();
    sig_mix(h, &pn, sizeof(pn));
    if (pn > 0) sig_mix(h, prims.data(), pn * sizeof(tracey::GeoPrimitive));
    // Shading-relevant per-corner / per-point attributes. Tag bytes
    // disambiguate "N on points" vs "N on vertices" so different storage
    // produces different hashes even with identical values.
    mix_vec3_attr(g.points().get<tracey::Vec3>("N"),  'n');
    mix_vec3_attr(g.vertices().get<tracey::Vec3>("N"), 'N');
    mix_vec2_attr(g.points().get<tracey::Vec2>("uv"),  'u');
    mix_vec2_attr(g.vertices().get<tracey::Vec2>("uv"), 'U');
    mix_vec3_attr(g.points().get<tracey::Vec3>("Cd"),  'c');
    mix_vec3_attr(g.vertices().get<tracey::Vec3>("Cd"), 'C');
    return h;
}

// Hash of just the TRS of an actor — flips on any transform tweak, stable
// otherwise. The fast path uses this to identify pure transform deltas.
uint64_t actor_transform_sig(const tracey::sops::EmittedActor &a) {
    uint64_t h = kSigOffset;
    sig_mix(h, &a.translate, sizeof(a.translate));
    sig_mix(h, &a.rotation, sizeof(a.rotation));
    sig_mix(h, &a.scale, sizeof(a.scale));
    // Instance groups fold every per-instance transform into a single
    // hash. A particle moving anywhere changes this; structural
    // count/tint changes go through actor_structural_sig instead.
    for (const auto &e : a.instances) {
        sig_mix(h, &e.translate, sizeof(e.translate));
        sig_mix(h, &e.rotation,  sizeof(e.rotation));
        sig_mix(h, &e.scale,     sizeof(e.scale));
    }
    return h;
}

// Hash of everything BUT the TRS — name, parent uid, light/subnet bits,
// material library, geometry positions. A diff in any of these forces
// the slow path (full scene rebuild).
uint64_t actor_structural_sig(const tracey::sops::EmittedActor &a) {
    uint64_t h = kSigOffset;
    sig_mix(h, &a.parentNodeUid, sizeof(a.parentNodeUid));
    sig_mix(h, &a.isSubnetMarker, sizeof(a.isSubnetMarker));
    sig_mix(h, &a.isLight, sizeof(a.isLight));
    sig_mix(h, &a.lightType, sizeof(a.lightType));
    sig_mix(h, &a.lightColor, sizeof(a.lightColor));
    sig_mix(h, &a.lightIntensity, sizeof(a.lightIntensity));
    sig_mix_str(h, a.name);
    sig_mix_str(h, a.materialLibraryName);
    // Per-instance tint folds into structural — a Cd-only template edit
    // changes only this slot but still has to invalidate the actor's
    // material so the new color reaches the materialBuffer.
    sig_mix(h, &a.hasTint, sizeof(a.hasTint));
    if (a.hasTint) sig_mix(h, &a.tint, sizeof(a.tint));
    // Instance-group count + per-instance tints are INTENTIONALLY NOT
    // folded in here. A particle sim spawning/dying entries every cook
    // would otherwise flip this hash and force the slow path —
    // compile_scene rebuilding materialBuffer + per-instance UV/program
    // walks for 40K particles at 60 Hz dominated rebuild_ms. The fast
    // TRS path resizes Actor.instances in place to absorb count changes;
    // per-instance tint changes show up as stale color until the next
    // structural change (stamp / material library / etc.) — a future
    // dedicated material-refresh path closes that gap.
    // Geometry digest — same shape as the dedup hash so a VOP-driven
    // edit to Cd / N / uv (point or vertex class) shows up as a
    // structural change. Previously this hashed only positions, which
    // meant an attribute_vop writing geo_output.Cd left the apply_emitted
    // fast path thinking nothing had changed and the rasterizer never
    // saw the new colors.
    const uint64_t geo = geometry_dedup_hash(a.geometry);
    sig_mix(h, &geo, sizeof(geo));
    return h;
}
}  // namespace

// Fingerprint of an emitted-actor list. Captures only the bits that affect
// the live scene — per-actor TRS, name/parent/material, light fields, and a
// shallow digest of the geometry data — so cook outputs that are byte-equal
// to the previous cook produce the same hash and apply_emitted can short-
// circuit. Excludes anything that would always differ (no pointers; no
// per-cook ordering noise beyond the emitted-vector order).
static uint64_t emitted_signature(const std::vector<tracey::sops::EmittedActor>& emitted) {
    constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime  = 0x00000100000001b3ULL;
    uint64_t h = kFnvOffset;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= kFnvPrime; }
    };
    auto mixStr = [&](const std::string& s) {
        mix(s.data(), s.size());
        // Length-suffix so "ab" + "c" doesn't collide with "a" + "bc".
        const uint64_t n = s.size();
        mix(&n, sizeof(n));
    };
    const uint32_t count = static_cast<uint32_t>(emitted.size());
    mix(&count, sizeof(count));
    for (const auto& a : emitted) {
        mix(&a.sourceNodeUid, sizeof(a.sourceNodeUid));
        mix(&a.parentNodeUid, sizeof(a.parentNodeUid));
        mix(&a.isSubnetMarker, sizeof(a.isSubnetMarker));
        mix(&a.isLight, sizeof(a.isLight));
        mix(&a.translate, sizeof(a.translate));
        mix(&a.rotation, sizeof(a.rotation));
        mix(&a.scale, sizeof(a.scale));
        mix(&a.lightType, sizeof(a.lightType));
        mix(&a.lightColor, sizeof(a.lightColor));
        mix(&a.lightIntensity, sizeof(a.lightIntensity));
        mixStr(a.name);
        mixStr(a.materialLibraryName);
        // Instance-group payload: count + per-instance TRS + tint. A
        // particle sim where only positions change has a.translate/.../
        // .scale all stuck at identity (the group sits at origin) — so
        // without this the signature would never flip and the
        // "nothing changed" early-out would freeze the viewport.
        const uint64_t ni = a.instances.size();
        mix(&ni, sizeof(ni));
        for (const auto &e : a.instances) {
            mix(&e.translate, sizeof(e.translate));
            mix(&e.rotation,  sizeof(e.rotation));
            mix(&e.scale,     sizeof(e.scale));
            mix(&e.hasTint,   sizeof(e.hasTint));
            if (e.hasTint) mix(&e.tint, sizeof(e.tint));
        }
        // Geometry digest — defer to geometry_dedup_hash so a VOP that
        // writes Cd / N / uv (point or vertex class) flips this
        // signature and forces apply_emitted to re-run. The earlier
        // "positions only" digest predates the rasterizer consuming
        // per-vertex colors and was silently swallowing any
        // attribute-only edit.
        const uint64_t geo = geometry_dedup_hash(a.geometry);
        mix(&geo, sizeof(geo));
    }
    return h;
}

void EditorServer::apply_emitted(std::vector<tracey::sops::EmittedActor>&& emitted) {
    if (!m_engine) return;

    // The geometry-hash cache is keyed by raw Geometry pointer. Within
    // this call the `emitted` vector holds shared_ptrs alive, so pointer
    // identity is sound; clearing here prevents a previous cook's freed
    // pointers (potentially re-used by a fresh allocation this cook)
    // from returning stale hashes.
    resetGeometryHashCache();

    // Houdini-style "nothing changed" early-out. The signature covers every
    // actor-visible field — if it matches the last applied cook, no actor
    // was added, removed, repositioned, renamed, or had its geometry edited
    // since last time. Skipping scene.clear() + compile_scene + BLAS/TLAS
    // rebuild means a Cmd+Z to the same state, a re-pushed graph with no
    // semantic change, or an unrelated edit elsewhere is effectively free.
    const uint64_t sig = emitted_signature(emitted);
    if (m_has_applied_once && sig == m_last_emitted_signature
        && m_engine->compiled_scene_ready()) {
        // Nothing observable changed — but apply_animation_at downstream
        // may still want to overlay animated transforms. We don't broadcast
        // scene_changed because the actor list is byte-identical to the
        // previous cook.
        return;
    }

    // Per-actor delta classification: build fresh sig pairs for each emitted
    // actor and compare against the previous apply. If every difference vs.
    // last time is a TRS-only delta (no actor added/removed, no structural
    // change), we can keep the existing scene + BLAS / material buffers
    // intact and just refresh the TLAS — orders of magnitude cheaper than
    // a full apply_emitted + compile_scene.
    std::unordered_map<uint64_t, ActorSig> newSigs;
    newSigs.reserve(emitted.size());
    // Each entry is (composite key, index into `emitted`) so the fast path
    // can directly pull the new TRS without a linear scan when there are
    // many instance actors.
    std::vector<std::pair<uint64_t, size_t>> moved_keys;
    bool fastPathEligible =
        m_has_applied_once && m_engine->compiled_scene_ready();
    for (size_t ei = 0; ei < emitted.size(); ++ei) {
        const auto &a = emitted[ei];
        if (a.sourceNodeUid == 0) {  // root-level emit with no source node — bail to slow path
            fastPathEligible = false;
        }
        const uint64_t key = make_actor_key(a);
        ActorSig s;
        s.transform_sig  = actor_transform_sig(a);
        s.structural_sig = actor_structural_sig(a);
        newSigs[key] = s;
        if (!fastPathEligible) continue;
        auto it = m_actor_signatures.find(key);
        if (it == m_actor_signatures.end()) {
            fastPathEligible = false;  // new actor — slow path
        } else if (it->second.structural_sig != s.structural_sig) {
            fastPathEligible = false;  // geometry / material / etc. changed
        } else if (it->second.transform_sig != s.transform_sig) {
            moved_keys.push_back({key, ei});
        }
    }
    // Removed actors (in last apply but not this one) also force the slow
    // path — scene needs to drop their Actors/SceneObjects.
    if (fastPathEligible) {
        for (const auto &[k, _] : m_actor_signatures) {
            if (newSigs.find(k) == newSigs.end()) {
                fastPathEligible = false;
                break;
            }
        }
    }

    if (fastPathEligible && !moved_keys.empty()) {
        // Transform-only fast path. Walk moved keys, push the new TRS onto
        // the existing live Actor, then ask the engine to rebuild ONLY the
        // TLAS from the current scene. compile_scene's heavy lifting (BLAS,
        // material buffer, UV buffer, program lookup) all stays.
        auto &liveScene = m_engine->scene();
        for (const auto &[key, ei] : moved_keys) {
            auto actorIt = m_emitted_actor_to_actor.find(key);
            if (actorIt == m_emitted_actor_to_actor.end()) continue;
            auto *actor = liveScene.getActor(actorIt->second);
            if (!actor) continue;
            const auto &a = emitted[ei];
            if (!a.instances.empty()) {
                // Instance group: the Actor sits at identity; per-particle
                // TRS lives in each SceneInstance's localTransform.
                // Particle birth/death changes the instance count, so
                // we resize the SceneInstance vector to match — cloning
                // the last existing slot's material into new entries (per-
                // instance tints stay stale until the next structural
                // refresh, which is the documented tradeoff for keeping
                // the fast path firing every cook).
                auto &slots = actor->instances();
                const size_t newN = a.instances.size();
                if (slots.size() < newN && !slots.empty()) {
                    slots.resize(newN, slots.back());
                } else if (slots.size() > newN) {
                    slots.resize(newN);
                }
                const size_t n = std::min(slots.size(), newN);
                // Per-slot writes touch only their own index — safe to
                // chunk across worker threads. At 400k particles this
                // drops the serial ~5–10 ms write loop to <1 ms on the
                // M3 Ultra. parallel_for_chunks falls back to a serial
                // body below ~1k entries so small scenes don't pay
                // thread-spawn overhead.
                const auto &entries = a.instances;
                tracey::parallel_for_chunks(n,
                    [&entries, &slots](size_t begin, size_t end) {
                        for (size_t i = begin; i < end; ++i) {
                            const auto &e = entries[i];
                            tracey::Transform xf;
                            xf.setPosition(e.translate);
                            xf.setRotation(tracey::Quaternion(e.rotation.x, e.rotation.y,
                                                               e.rotation.z, e.rotation.w));
                            xf.setScale(e.scale);
                            slots[i].setLocalTransform(xf);
                        }
                    });
            } else {
                tracey::Transform xf;
                xf.setPosition(a.translate);
                xf.setRotation(tracey::Quaternion(a.rotation.x, a.rotation.y,
                                                   a.rotation.z, a.rotation.w));
                xf.setScale(a.scale);
                actor->setTransform(xf);
            }
        }
        if (m_engine->refresh_tlas_only()) {
            m_actor_signatures = std::move(newSigs);
            m_last_emitted_signature = sig;
            m_has_applied_once = true;
            m_clear_next_frame = true;
            if (m_broadcast) m_broadcast(R"({"event":"scene_changed"})");
            // Animation overlay still wants to re-run after a transform
            // update so any animated channels write fresh values on top.
            m_timeline_dirty = true;
            return;
        }
        // refresh_tlas_only returned false (topology drifted, e.g. a hidden
        // actor flipped visible between compile and apply). Fall through
        // to the slow path.
    }

    m_last_emitted_signature = sig;
    m_has_applied_once = true;

    // The camera state is independent of the SOP graph (driven by user
    // fly-through) so we preserve it across cooks; otherwise every parameter
    // tweak would snap the viewport back to the default angle.
    auto& scene = m_engine->scene();

    // Incremental slow path: rather than `scene.clear()` + reconstruct
    // from scratch, diff vs. m_actor_signatures and only recreate the
    // actors / SceneObjects whose structural_sig changed. Unchanged
    // actors keep their existing Actor + SceneObject + child links, so
    // we skip the GeometryConverter::toSceneObject vertex-copy and the
    // Scene::createActor allocation for them entirely. The BLAS cache
    // handles BVH reuse on the compile_scene side.
    //
    // Removed actors (uid present in m_actor_signatures but not in
    // newSigs) get scene.removeActor + scene.removeObject. Added actors
    // (key present in newSigs but not in m_actor_signatures) take the
    // create path below. Structural-change actors are remove-then-create.
    std::unordered_set<uint64_t> newKeys;
    std::unordered_set<size_t> newUids;
    newKeys.reserve(emitted.size());
    for (const auto &a : emitted) {
        newKeys.insert(make_actor_key(a));
        newUids.insert(a.sourceNodeUid);
    }

    // Helper: drop the SceneObject reference owned by `key` from the
    // refcount + content-hash table. Removes the SceneObject from the live
    // scene only when no other actor still references it (refcount → 0).
    auto release_object_for_key = [&](uint64_t key) {
        auto objIt = m_sop_node_object_names.find(key);
        if (objIt == m_sop_node_object_names.end()) return;
        const std::string name = objIt->second;
        m_sop_node_object_names.erase(objIt);
        auto rcIt = m_scene_object_refcount.find(name);
        if (rcIt == m_scene_object_refcount.end()) return;
        if (--rcIt->second > 0) return;
        m_scene_object_refcount.erase(rcIt);
        scene.removeObject(name);
        // Drop the reverse content-hash entry that pointed at this name.
        for (auto h2n = m_geometry_hash_to_object_name.begin();
             h2n != m_geometry_hash_to_object_name.end();)
        {
            if (h2n->second == name) h2n = m_geometry_hash_to_object_name.erase(h2n);
            else ++h2n;
        }
    };

    // Iterate the *old* m_actor_signatures (still holds the previous
    // cook's set) and prune anything that isn't in this cook's newKeys.
    // This is where actors from a deleted SOP (e.g. removing an Instance
    // SOP that had emitted N TLAS instances) get torn down. The new
    // signature map is swapped in AFTER this diff so we still have the
    // old set to walk against.
    for (auto it = m_actor_signatures.begin(); it != m_actor_signatures.end();)
    {
        if (newKeys.find(it->first) != newKeys.end()) { ++it; continue; }
        auto actorIt = m_emitted_actor_to_actor.find(it->first);
        if (actorIt != m_emitted_actor_to_actor.end()) {
            scene.removeActor(actorIt->second);
            // Also drop the convenience sourceNodeUid→actor mapping when
            // the removed actor is the primary (instanceIndex==0).
            const size_t uid = static_cast<size_t>(it->first >> 24);
            const uint32_t inst = static_cast<uint32_t>(it->first & 0xFFFFFFULL);
            if (inst == 0) m_sop_node_to_actor.erase(uid);
            m_emitted_actor_to_actor.erase(actorIt);
        }
        release_object_for_key(it->first);
        // Only forget the per-uid visibility flag when *every* actor for
        // that uid is gone (instance SOPs share one visibility flag across
        // all their instances).
        const size_t uid = static_cast<size_t>(it->first >> 24);
        if (newUids.find(uid) == newUids.end()) {
            m_sop_node_visible.erase(uid);
        }
        it = m_actor_signatures.erase(it);
    }
    // NOTE: m_actor_signatures is intentionally NOT swapped here yet —
    // the per-actor create loop below still needs to look up each new
    // key's *previous* signature to decide between "unchanged → skip",
    // "transform-only → just retarget", and "structural change →
    // remove-and-recreate". The swap happens after the create loop.

    // Pass 1: create one Actor per emit. Subnet markers create transform-only
    // actors (no SceneObject, no SceneInstance, no material) — they're parent
    // transform nodes in the scene tree. SopGraph::cook() pushes subnet
    // markers before their children, so the parent uid is already in
    // m_sop_node_to_actor when pass 2 wires the parent links.
    // Helper: restore the per-actor display flag for the actor that was just
    // emitted from this SOP node. Default visible when the map has no entry.
    auto restoreVisibility = [&](tracey::Actor* actor, size_t sourceNodeUid) {
        if (!actor || sourceNodeUid == 0) return;
        auto it = m_sop_node_visible.find(sourceNodeUid);
        if (it != m_sop_node_visible.end()) actor->setVisible(it->second);
    };

    // ── glTF material / texture forwarding ──────────────────────────────
    // gltf_import SOPs stamp the source path + mesh name as Detail attrs on
    // the geometry. We use those here to (a) copy the source's embedded
    // textures into the live scene under the same "embedded:N" ids the
    // material refers to, and (b) pull the matching SceneInstance's
    // MaterialInstance and apply it to the live actor instead of the
    // flat-grey default. Without this an imported glTF renders untextured.
    //
    // Cache by path so a multi-primitive glTF only re-parses once per cook
    // apply. The cache is local to this call — released when apply_emitted
    // returns, so we don't pin loaded scenes between cooks.
    auto getSourceScene = [&](const std::string& path)
        -> std::shared_ptr<const tracey::Scene> {
        if (path.empty()) return nullptr;
        try {
            // Shares the process-wide cache with GltfImportSop::cook, so the
            // editor never re-parses a file just to recover its materials.
            return tracey::GltfLoader::loadFromFileCached(path);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[apply_emitted] glTF material load failed for %s: %s\n",
                path.c_str(), e.what());
            return nullptr;
        }
    };

    auto pullGltfMaterial = [&](const tracey::Geometry& geo,
                                tracey::MaterialInstance* out) -> bool {
        const auto* pathAttr = geo.detail().get<std::string>("_gltf_source_path");
        const auto* meshAttr = geo.detail().get<std::string>("_gltf_source_mesh");
        if (!pathAttr || !meshAttr) return false;
        if (pathAttr->data().empty() || meshAttr->data().empty()) return false;
        const std::string& path = pathAttr->data()[0];
        const std::string& meshName = meshAttr->data()[0];
        auto src = getSourceScene(path);
        if (!src) return false;

        // Mirror every embedded texture id from the source into the live
        // scene so the renderer's `getEmbeddedTexture("embedded:N")` lookup
        // resolves. Scene::addEmbeddedTexture is idempotent on duplicate ids
        // — repeat copies for shared meshes are harmless.
        for (const auto& [id, tex] : src->embeddedTextures()) {
            if (!scene.hasEmbeddedTexture(id)) {
                tracey::EmbeddedTexture copy = tex;
                scene.addEmbeddedTexture(id, std::move(copy));
            }
        }

        // Walk source actors → find the SceneInstance whose objectRef
        // matches our mesh name; that instance carries the glTF material.
        for (const auto& a : src->actors()) {
            if (!a) continue;
            for (const auto& inst : a->instances()) {
                if (inst.objectRef() == meshName) {
                    *out = inst.material();
                    return true;
                }
            }
        }
        return false;
    };

    // Composite keys whose Actor got newly created (or recreated after a
    // structural change) in this pass. Used by Pass 2 below to gate parent
    // re-wiring — addChild isn't idempotent, so re-running it on unchanged
    // actors would push duplicate child uids.
    std::unordered_set<uint64_t> recreatedKeys;

    for (const auto& ea : emitted) {
        const uint64_t actorKey = make_actor_key(ea);
        // Skip-when-unchanged: if this composite key already has a live
        // actor in the scene with the same structural_sig, the actor's
        // SceneObject / material / instance bookkeeping is still valid.
        // We only need to update its transform if transform_sig differs —
        // compile_scene below picks up actor->transform() into the new
        // TLAS.
        const auto sigIt = m_actor_signatures.find(actorKey);
        const ActorSig &newSig = newSigs[actorKey];
        if (sigIt != m_actor_signatures.end() &&
            sigIt->second.structural_sig == newSig.structural_sig)
        {
            if (sigIt->second.transform_sig != newSig.transform_sig)
            {
                auto actorIt = m_emitted_actor_to_actor.find(actorKey);
                if (actorIt != m_emitted_actor_to_actor.end()) {
                    auto *actor = scene.getActor(actorIt->second);
                    if (actor) {
                        if (!ea.instances.empty()) {
                            // Instance group — resize + update per-particle
                            // local transforms in place. Mirror of the
                            // global fast path branch.
                            auto &slots = actor->instances();
                            const size_t newN = ea.instances.size();
                            if (slots.size() < newN && !slots.empty()) {
                                slots.resize(newN, slots.back());
                            } else if (slots.size() > newN) {
                                slots.resize(newN);
                            }
                            const size_t n = std::min(slots.size(), newN);
                            const auto &entries = ea.instances;
                            tracey::parallel_for_chunks(n,
                                [&entries, &slots](size_t begin, size_t end) {
                                    for (size_t i = begin; i < end; ++i) {
                                        const auto &e = entries[i];
                                        tracey::Transform xf;
                                        xf.setPosition(e.translate);
                                        xf.setRotation(tracey::Quaternion(e.rotation.x, e.rotation.y,
                                                                           e.rotation.z, e.rotation.w));
                                        xf.setScale(e.scale);
                                        slots[i].setLocalTransform(xf);
                                    }
                                });
                        } else {
                            tracey::Transform xf;
                            xf.setPosition(ea.translate);
                            xf.setRotation(tracey::Quaternion(ea.rotation.x, ea.rotation.y,
                                                               ea.rotation.z, ea.rotation.w));
                            xf.setScale(ea.scale);
                            actor->setTransform(xf);
                        }
                    }
                }
            }
            continue;
        }

        // Structural change OR brand-new key: tear down any prior actor +
        // release this key's SceneObject reference (the shared SceneObject
        // only drops out of the scene when no other actor still uses it).
        {
            auto actorIt = m_emitted_actor_to_actor.find(actorKey);
            if (actorIt != m_emitted_actor_to_actor.end()) {
                scene.removeActor(actorIt->second);
                if (ea.instanceIndex == 0) m_sop_node_to_actor.erase(ea.sourceNodeUid);
                m_emitted_actor_to_actor.erase(actorIt);
            }
            release_object_for_key(actorKey);
        }
        recreatedKeys.insert(actorKey);

        if (ea.isSubnetMarker) {
            auto* actor = scene.createActor();
            actor->setName(ea.name.empty() ? "subnet" : ea.name);
            tracey::Transform xform;
            xform.setPosition(ea.translate);
            xform.setRotation(tracey::Quaternion(ea.rotation.x, ea.rotation.y,
                                                 ea.rotation.z, ea.rotation.w));
            xform.setScale(ea.scale);
            actor->setTransform(xform);
            if (ea.sourceNodeUid != 0) {
                m_emitted_actor_to_actor[actorKey] = actor->getUid();
                if (ea.instanceIndex == 0)
                    m_sop_node_to_actor[ea.sourceNodeUid] = actor->getUid();
            }
            restoreVisibility(actor, ea.sourceNodeUid);
            continue;
        }

        if (ea.isLight) {
            // Houdini-style /obj light. Transform-only actor with a Light
            // component attached; no SceneObject, no SceneInstance. The
            // SceneCompiler picks light actors up into a separate light
            // list later in the wavefront SSBO bind.
            auto* actor = scene.createActor();
            actor->setName(ea.name.empty() ? "light" : ea.name);
            tracey::Transform xform;
            xform.setPosition(ea.translate);
            xform.setRotation(tracey::Quaternion(ea.rotation.x, ea.rotation.y,
                                                 ea.rotation.z, ea.rotation.w));
            xform.setScale(ea.scale);
            actor->setTransform(xform);

            tracey::Light light;
            light.type = (ea.lightType == 1) ? tracey::LightType::Distant
                                             : tracey::LightType::Point;
            light.color = ea.lightColor;
            light.intensity = ea.lightIntensity;
            actor->setLight(light);

            if (ea.sourceNodeUid != 0) {
                m_emitted_actor_to_actor[actorKey] = actor->getUid();
                if (ea.instanceIndex == 0)
                    m_sop_node_to_actor[ea.sourceNodeUid] = actor->getUid();
            }
            restoreVisibility(actor, ea.sourceNodeUid);
            continue;
        }

        // Each emitted actor's geometry becomes a SceneObject — but only if
        // we haven't already added an identical Geometry in this process.
        // Two emit sources with the same vertex content share ONE
        // SceneObject (and therefore one vertex buffer, color buffer, and
        // BLAS), bumping the refcount so the SceneObject only goes away
        // when the last referencing actor does.
        if (!ea.geometry) continue;  // defensive: real-geo branch guards above
                                     // should have already skipped lights /
                                     // subnet markers.
        const uint64_t geomHash = geometry_dedup_hash(ea.geometry);
        std::string objectName;
        auto dedupIt = m_geometry_hash_to_object_name.find(geomHash);
        if (dedupIt != m_geometry_hash_to_object_name.end() &&
            scene.hasObject(dedupIt->second))
        {
            // Cache hit: reuse the existing SceneObject. We deliberately
            // re-verify it's still in the live scene — a previous teardown
            // could have evicted it but left the hash entry behind in
            // pathological cases.
            objectName = dedupIt->second;
        }
        else
        {
            // Fresh content: build a new SceneObject. Suffix the uid so
            // two distinct-content actors with the same display name (e.g.
            // multiple `Body_prim_0`s) don't clobber each other in
            // scene.m_objects.
            objectName = (ea.name.empty() ? std::string{"actor"} : ea.name) +
                         "_" + std::to_string(ea.sourceNodeUid);
            scene.addObject(objectName, tracey::GeometryConverter::toSceneObject(*ea.geometry, objectName));
            m_geometry_hash_to_object_name[geomHash] = objectName;
        }
        m_sop_node_object_names[actorKey] = objectName;
        ++m_scene_object_refcount[objectName];

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
        // graph JSON now so the next compile_scene picks it up. Track the
        // library name on the Actor too so the save-material handler can
        // refresh just the bound actors without re-cooking the SOP graph.
        if (!ea.materialLibraryName.empty() &&
            is_safe_library_name(ea.materialLibraryName)) {
            // resolve_material_path checks the project's local
            // materials folder first and falls back to the global
            // library. A project that ships its own copy of a
            // same-named material now shadows the global one.
            std::ifstream in(resolve_material_path(ea.materialLibraryName));
            if (in) {
                std::stringstream ss;
                ss << in.rdbuf();
                actor->setMaterialGraphJson(ss.str());
                actor->setMaterialLibraryName(ea.materialLibraryName);
            }
        } else {
            // Empty SOP-side material name means "no graph assigned" —
            // clear both the library name AND the graph JSON. Leaving
            // the graph behind would let a stale material from a
            // previous cook keep applying after the user has cleared
            // the assignment, even though the SOP graph no longer
            // references it.
            actor->setMaterialGraphJson({});
            actor->setMaterialLibraryName({});
        }

        // Default material instance — replaced below by the glTF source's
        // material when the geometry carries the `_gltf_source_*` tags.
        tracey::MaterialInstance mat("pbr");
        mat.setAlbedo(tracey::Vec3(0.8f));
        mat.setMetallic(0.0f);
        mat.setRoughness(0.5f);
        // Honour the user's library override (set above via
        // setMaterialGraphJson) by leaving the flat default in place — the
        // material graph wins. Otherwise, prefer the glTF-source material so
        // imported actors render with their textures + factors.
        if (ea.materialLibraryName.empty() ||
            !is_safe_library_name(ea.materialLibraryName)) {
            tracey::MaterialInstance fromGltf("pbr");
            if (pullGltfMaterial(*ea.geometry, &fromGltf)) {
                mat = fromGltf;
            }
        }
        // Per-instance albedo tint (Phase C of GPU instancing). When the
        // upstream cook attached a tint — e.g. `instance` SOP forwarding
        // a per-point Cd from the template — multiply it into the base
        // material albedo. Each TLAS instance gets its own materialBuffer
        // entry, so 1000 instances sharing one BLAS still get 1000
        // distinct colors. Skip when a material library is in play — the
        // material graph already controls albedo through its own knobs.
        const bool tintAllowed = ea.materialLibraryName.empty() ||
                                  !is_safe_library_name(ea.materialLibraryName);
        if (ea.hasTint && tintAllowed) {
            const tracey::Vec3 base = mat.albedo().value_or(tracey::Vec3(1.0f));
            mat.setAlbedo(tracey::Vec3(base.x * ea.tint.x,
                                        base.y * ea.tint.y,
                                        base.z * ea.tint.z));
        }

        if (!ea.instances.empty()) {
            // Instance-group emit: one Actor sitting at identity, N
            // SceneInstances each carrying its own local transform and
            // per-particle tinted material. compile_scene's per-instance
            // loop walks these to build N TLAS entries pointing at the
            // shared BLAS, exactly the GPU-instancing topology we want.
            // The Actor's own transform stays identity (set above to
            // ea.translate/.../scale, which are also identity for an
            // instance emit — the cook side leaves them at defaults).
            for (const auto &e : ea.instances) {
                tracey::MaterialInstance imat = mat;
                if (e.hasTint && tintAllowed) {
                    const tracey::Vec3 base = imat.albedo().value_or(tracey::Vec3(1.0f));
                    imat.setAlbedo(tracey::Vec3(base.x * e.tint.x,
                                                base.y * e.tint.y,
                                                base.z * e.tint.z));
                }
                tracey::SceneInstance si(objectName, imat);
                tracey::Transform xf;
                xf.setPosition(e.translate);
                xf.setRotation(tracey::Quaternion(e.rotation.x, e.rotation.y,
                                                   e.rotation.z, e.rotation.w));
                xf.setScale(e.scale);
                si.setLocalTransform(xf);
                actor->addInstance(std::move(si));
            }
        } else {
            actor->addInstance(tracey::SceneInstance(objectName, mat));
        }

        // Stable actor↔SOP mapping via the composite key threaded through
        // EmittedActor. The actor we just appended is at the back of
        // scene.actors().
        if (ea.sourceNodeUid != 0 && !scene.actors().empty()) {
            const uint64_t newActorUid = scene.actors().back()->getUid();
            m_emitted_actor_to_actor[actorKey] = newActorUid;
            if (ea.instanceIndex == 0)
                m_sop_node_to_actor[ea.sourceNodeUid] = newActorUid;
        }
        restoreVisibility(actor, ea.sourceNodeUid);
    }

    // Pass 2: wire parent → child edges for recreated actors only. Both
    // directions are stored so Scene::flatten() can walk top-down without
    // a per-actor reverse scan. Unchanged actors keep their existing
    // parent/child links — addChild isn't idempotent, so re-running it
    // every cook on those would duplicate child uids. Parenting is
    // expressed in SOP-node terms (parentNodeUid → sourceNodeUid), so we
    // use the SOP→primary-actor map on both sides.
    for (const auto& ea : emitted) {
        if (ea.parentNodeUid == 0) continue;
        const uint64_t actorKey = make_actor_key(ea);
        if (recreatedKeys.find(actorKey) == recreatedKeys.end()) continue;
        const auto childIt = m_emitted_actor_to_actor.find(actorKey);
        const auto parentIt = m_sop_node_to_actor.find(ea.parentNodeUid);
        if (childIt == m_emitted_actor_to_actor.end() ||
            parentIt == m_sop_node_to_actor.end()) {
            continue;
        }
        auto* child = scene.getActor(childIt->second);
        auto* parent = scene.getActor(parentIt->second);
        if (!child || !parent) continue;
        parent->addChild(child);
        child->setParent(parent->getUid());
    }

    // Both diff passes have read m_actor_signatures (previous state) and
    // newSigs (this cook's state). Promote newSigs to the canonical
    // m_actor_signatures now that we're done diffing.
    m_actor_signatures = std::move(newSigs);

    // Recompile so the path tracer picks up the new BLAS/TLAS + material
    // programs, and reset accumulation.
    if (m_engine->path_tracer_ready()) {
        m_engine->compile_scene();
        m_clear_next_frame = true;
    }

    if (m_broadcast) {
        m_broadcast(R"({"event":"scene_changed"})");
    }

    // Recompute whether the graph has any attribute_vop with animated
    // promoted host params. Drives the auto re-cook on time change in
    // render_tick.
    m_has_animated_vop_promotions = detect_animated_vop_promotions();

    // Same idea for dop_import — when present, every playhead move needs
    // to cook the DOP forward and re-stamp + re-cook the SOP graph.
    m_has_dop_imports = detect_dop_imports();

    // Cook reset every actor's transform to its SOP-constant baseline; the
    // next render_tick re-evaluates animated overrides on top.
    m_timeline_dirty = true;
}

// ── Timeline / animation override ──────────────────────────────────────────

namespace {

// Convert a frame index to seconds at the given fps. Frame numbering follows
// Houdini convention (1-based) but the math holds for any base — the UI is
// the only thing that cares about the offset.
inline double seconds_for_frame_(double frame, double fps) {
    return fps > 0.0 ? (frame - 1.0) / fps : 0.0;
}

// Inclusive frame range [start, end] in seconds. End is exclusive when used
// as a wrap point (a key at frame_end+1 sits at end_seconds).
inline double range_start_seconds(int frame_start, double fps) {
    return seconds_for_frame_(double(frame_start), fps);
}
inline double range_end_seconds(int frame_end, double fps) {
    return seconds_for_frame_(double(frame_end + 1), fps);
}

}  // namespace

// Step the playhead forward (or backward, in PingPong) by `dt` seconds,
// applying the active LoopMode. Returns true when current_time actually
// changed. Shared between the wall-clock async path (called from
// render_tick with dt = elapsed wall time) and the frame-locked path
// (called from drain_cook_result with dt = 1/fps once a cook lands).
bool EditorServer::advance_playhead_by(double dt) {
    if (!m_timeline.playing || dt <= 0.0) return false;
    const double t0 = range_start_seconds(m_timeline.frame_start, m_timeline.fps);
    const double t1 = range_end_seconds(m_timeline.frame_end, m_timeline.fps);
    const double range = std::max(t1 - t0, 1.0 / std::max(m_timeline.fps, 1.0));

    double t = m_timeline.current_time + dt * m_timeline.pingpong_dir;

    switch (m_timeline.loop) {
        case LoopMode::Once:
            if (t >= t1) {
                t = t1;
                m_timeline.playing = false;
            } else if (t < t0) {
                t = t0;
            }
            break;
        case LoopMode::Loop: {
            if (t >= t1 || t < t0) {
                double off = std::fmod(t - t0, range);
                if (off < 0.0) off += range;
                t = t0 + off;
            }
            break;
        }
        case LoopMode::PingPong: {
            if (t >= t1) {
                t = t1 - (t - t1);
                m_timeline.pingpong_dir = -1.0;
            } else if (t < t0) {
                t = t0 + (t0 - t);
                m_timeline.pingpong_dir = 1.0;
            }
            break;
        }
    }
    if (t == m_timeline.current_time) return false;
    m_timeline.current_time = t;
    return true;
}

bool EditorServer::advance_playhead(double dt) {
    // Frame-locked playback advances inside drain_cook_result instead,
    // one frame at a time after each cook completion. Wall-clock dt
    // would skip frames whenever the cook is slower than realtime, so
    // we suppress it here.
    if (m_timeline.frame_locked) return false;
    return advance_playhead_by(dt);
}

// Walk the actor↔SOP map, evaluate animated translate/rotate/scale params at
// `time`, and override the corresponding actor transforms. The path tracer +
// rasterizer both render from the cached `m_compiled_scene` (TLAS instance
// transforms are baked at compile-time), so we must recompile after touching
// any transform — the same way the inspector slider path does it (App.tsx →
// set_actor_transform → compileScene).
//
// Rotation is stored on the SOP node as per-axis euler-degrees and converted
// to quaternion (ZYX intrinsic, matching transform_sop and the cook emit
// path) here. Per-component channels animate naturally — same shape as
// translate/scale — at the cost of gimbal lock at extremes (Houdini lives
// with this and so do we for v1).
//
// Other animated params (primitive sizes, etc.) need a full re-cook to take
// effect, not just a recompile; for v1 we only handle object_output / subnet
// transforms.
void EditorServer::apply_animation_at(double time) {
    if (!m_sop_graph || !m_engine) return;

    bool any_changed = false;
    for (const auto& [outputUid, actorUid] : m_sop_node_to_actor) {
        auto* node = findNodeRecursive(m_sop_graph.get(),outputUid);
        if (!node) continue;
        auto* actor = m_engine->scene().getActor(actorUid);
        if (!actor) continue;

        const tracey::sops::Parameter* pT = nullptr;
        const tracey::sops::Parameter* pR = nullptr;
        const tracey::sops::Parameter* pS = nullptr;
        for (const auto& p : node->parameters()) {
            if      (p.name == "translate")        pT = &p;
            else if (p.name == "rotate_euler_deg") pR = &p;
            else if (p.name == "scale")            pS = &p;
        }
        const bool tAnim = pT && pT->isAnimated();
        const bool rAnim = pR && pR->isAnimated();
        const bool sAnim = pS && pS->isAnimated();
        if (!tAnim && !rAnim && !sAnim) continue;

        tracey::Transform xf = actor->transform();
        if (tAnim) {
            auto v = pT->evaluateAt(time);
            if (auto* vec = std::get_if<tracey::Vec3>(&v)) xf.setPosition(*vec);
        }
        if (rAnim) {
            auto v = pR->evaluateAt(time);
            if (auto* deg = std::get_if<tracey::Vec3>(&v)) {
                constexpr float kDeg2Rad = 3.1415926535f / 180.0f;
                const tracey::Vec3 rad = *deg * kDeg2Rad;
                glm::quat qx = glm::angleAxis(rad.x, glm::vec3(1, 0, 0));
                glm::quat qy = glm::angleAxis(rad.y, glm::vec3(0, 1, 0));
                glm::quat qz = glm::angleAxis(rad.z, glm::vec3(0, 0, 1));
                xf.setRotation(qz * qy * qx);
            }
        }
        if (sAnim) {
            auto v = pS->evaluateAt(time);
            if (auto* vec = std::get_if<tracey::Vec3>(&v)) xf.setScale(*vec);
        }
        actor->setTransform(xf);
        any_changed = true;
    }
    if (any_changed) {
        m_clear_next_frame = true;
        // Rebuild the TLAS with the new instance transforms. Without this the
        // path tracer + rasterizer keep rendering the cached compile-time
        // transforms and the override is invisible.
        if (m_engine->path_tracer_ready() && m_engine->compiled_scene_ready()) {
            try {
                m_engine->compile_scene();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[anim] compile_scene failed: %s\n", e.what());
            }
        }
    }
}

// Walks the canonical SOP graph (recursing into subnet inner graphs) and
// returns true if any attribute_vop's promoted host param is animated. The
// auto re-cook in render_tick uses this to decide whether to re-cook on
// playhead change. Called from apply_emitted after every cook completion.
bool EditorServer::detect_animated_vop_promotions() const {
    if (!m_sop_graph) return false;
    std::function<bool(const tracey::sops::SopGraph*)> walk;
    walk = [&](const tracey::sops::SopGraph* g) -> bool {
        if (!g) return false;
        for (const auto& n : g->nodes()) {
            auto* sn = dynamic_cast<const tracey::sops::SopNode*>(n.get());
            if (!sn) continue;
            if (sn->kind() == "attribute_vop") {
                if (const auto* proms = tracey::sops::attributeVopPromotions(sn)) {
                    for (const auto& p : *proms) {
                        for (const auto& q : sn->parameters()) {
                            if (q.name == p.hostParamName && q.isAnimated()) {
                                return true;
                            }
                        }
                    }
                }
            }
            if (sn->innerGraph() && walk(sn->innerGraph())) return true;
        }
        return false;
    };
    return walk(m_sop_graph.get());
}

// True iff the canonical graph carries a dop_import SOP anywhere (including
// inside subnets). The playhead-driven DOP re-cook in render_tick reads
// this so projects without particles don't pay the cost on every scrub.
bool EditorServer::detect_dop_imports() const {
    if (!m_sop_graph) return false;
    std::function<bool(const tracey::sops::SopGraph*)> walk;
    walk = [&](const tracey::sops::SopGraph* g) -> bool {
        if (!g) return false;
        for (const auto& n : g->nodes()) {
            auto* sn = dynamic_cast<const tracey::sops::SopNode*>(n.get());
            if (!sn) continue;
            if (sn->kind() == "dop_import") return true;
            if (sn->innerGraph() && walk(sn->innerGraph())) return true;
        }
        return false;
    };
    return walk(m_sop_graph.get());
}

// Cook the DopGraph to the frame matching `time` (cheap when already
// cached), then walk the SOP graph collecting (uid, current-frame
// geometry) pairs for every dop_import SOP. Caller must hold m_mutex.
// Returns empty when there's nothing to stamp.
std::vector<std::pair<size_t, tracey::Geometry>>
EditorServer::collect_dop_stamps(double time) {
    std::vector<std::pair<size_t, tracey::Geometry>> out;
    if (!m_dop_graph || !m_sop_graph || !m_has_dop_imports) return out;

    // 1-based frame index. time=0 → frame 1, etc. Match the convention
    // in DopGraph::cookOneFrame (header.time = (frameIdx - 1) / fps).
    //
    // The epsilon nudge is load-bearing: the video export computes
    // `frame_time = (f - 1) / fps`, then we recover the frame index via
    // `floor(frame_time * fps) + 1`. In IEEE-754 doubles `(1/24)*24`
    // rounds to ~0.99999…, not 1.0, so the naive floor() gives 0 and
    // every export frame past the first reads frame 1's sim state —
    // i.e. the entire exported video shows the same first-frame
    // particle snapshot. The live viewport doesn't hit this because
    // the playhead time is set continuously by the scrub, not derived
    // from an integer frame.
    const double fps = std::max(m_timeline.fps, 1e-6);
    const int frame_idx = std::max(1,
        static_cast<int>(std::floor(time * fps + 1e-6)) + 1);

    // Scrubbing backward (or graph edited) requires re-sim from the
    // empty baseline. cookToFrame is monotonic; clear first when we're
    // landing in the past.
    const int prev_cached = m_dop_graph->cachedToFrame();
    if (frame_idx < prev_cached) {
        m_dop_graph->clearCache();
    }
    m_dop_graph->cookToFrame(frame_idx, fps);

    // Broadcast cache-extent change so the dopesheet UI can draw a
    // "cached up to frame N" indicator. Cheap: only fires when the
    // extent actually moved.
    const int new_cached = m_dop_graph->cachedToFrame();
    if (m_broadcast && new_cached != prev_cached) {
        json msg = {
            {"event", "dop_status"},
            {"cached_to_frame", new_cached},
            {"current_frame",   frame_idx},
        };
        m_broadcast(msg.dump());
    }

    const tracey::dops::SimState* state = m_dop_graph->frame(frame_idx);
    if (!state) return out;

    // Collect uids of every dop_import node in m_sop_graph, recursing.
    std::vector<size_t> uids;
    std::function<void(const tracey::sops::SopGraph*)> walk;
    walk = [&](const tracey::sops::SopGraph* g) {
        if (!g) return;
        for (const auto& n : g->nodes()) {
            auto* sn = dynamic_cast<const tracey::sops::SopNode*>(n.get());
            if (!sn) continue;
            if (sn->kind() == "dop_import") uids.push_back(sn->uid());
            if (sn->innerGraph()) walk(sn->innerGraph());
        }
    };
    walk(m_sop_graph.get());

    out.reserve(uids.size());
    for (size_t uid : uids) {
        // Same Geometry payload for every importer in v1 — Phase N could
        // add a "which actor / which DOP network" selector and split here.
        out.emplace_back(uid, state->geometry);
    }
    return out;
}

// True if any SOP node anywhere in the graph carries an animated parameter
// (translate/rotate/scale on a subnet, an animated VOP promotion, a
// keyframed scalar on any leaf, etc.). Used by the export loop to decide
// whether per-frame state can be skipped on a static scene.
bool EditorServer::detect_any_animation() const {
    if (!m_sop_graph) return false;
    // A DOP-driven scene is animated even if no SOP parameter is keyed —
    // particle state changes every frame from the simulation itself.
    // Without this, the video export's `scene_static` short-circuit
    // skips the per-frame re-cook and every frame after the first
    // renders with stale dop_import geometry — the "exported video
    // freezes at frame 1" symptom.
    if (m_has_dop_imports) return true;
    std::function<bool(const tracey::sops::SopGraph*)> walk;
    walk = [&](const tracey::sops::SopGraph* g) -> bool {
        if (!g) return false;
        for (const auto& n : g->nodes()) {
            auto* sn = dynamic_cast<const tracey::sops::SopNode*>(n.get());
            if (!sn) continue;
            for (const auto& p : sn->parameters()) {
                if (p.isAnimated()) return true;
            }
            if (sn->innerGraph() && walk(sn->innerGraph())) return true;
        }
        return false;
    };
    return walk(m_sop_graph.get());
}

// Worker thread: pull JSON requests, deserialize + cook on a private
// SopGraph copy, hand the emitted actor list to the main thread via
// m_pending_cook_result. Latest-wins: a new set_sop_graph while a cook is
// running overwrites the pending request, and the worker only ever cooks
// the most recently posted JSON.
void EditorServer::cook_worker_loop() {
    while (true) {
        CookRequest request;
        {
            std::unique_lock<std::mutex> lk(m_cook_request_mutex);
            m_cook_request_cv.wait(lk, [&] {
                return m_cook_shutdown || m_pending_cook_request.has_value();
            });
            if (m_cook_shutdown) return;
            request = std::move(*m_pending_cook_request);
            m_pending_cook_request.reset();
        }

        std::unique_ptr<tracey::sops::SopGraph> graph;
        try {
            graph = tracey::sops::deserializeSopGraph(request.graph_json);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[sop worker] parse failed: %s\n", e.what());
            continue;
        }
        if (!graph) continue;

        // Apply DOP-import side-channel stamps. The serialized graph drops
        // dop_import's m_stamped Geometry; the canonical EditorServer side
        // collected the current-frame geometries and passed them through
        // CookRequest::dop_stamps. We poke them into the worker's private
        // graph clone here so cook() sees real data.
        for (auto& [uid, geo] : request.dop_stamps) {
            if (auto* node = findNodeRecursive(graph.get(), uid)) {
                tracey::sops::setDopImportGeometry(node, std::move(geo));
            }
        }

        tracey::sops::CookDiagnostic diag;
        std::vector<tracey::sops::EmittedActor> emitted;
        std::vector<tracey::sops::NodeCookTiming> timings;
        try {
            // Houdini-style cook cache: per-node Geometry cache keyed by
            // (kind, params hash, upstream cookIds, time if time-dep). The
            // worker thread owns the cache exclusively, so no lock needed
            // — the loop only ever runs one cook at a time. Mark all
            // untouched up-front and evict after the cook so entries from
            // a node the user deleted get freed.
            m_worker_cook_cache.markAllUntouched();
            emitted = graph->cook(&diag, request.time, &m_worker_cook_cache, &timings);
            m_worker_cook_cache.evictUntouched();
        } catch (const std::exception& e) {
            // Without this catch, an uncaught exception from any SOP's cook()
            // would terminate the worker thread for the lifetime of the
            // process, leaving subsequent edits silently uncooked. Log and
            // keep the worker alive.
            std::fprintf(stderr, "[sop worker] cook threw: %s\n", e.what());
            continue;
        }
        if (!diag.ok) {
            std::fprintf(stderr, "[sop worker] cook failed: %s (node uid=%zu)\n",
                         diag.message.c_str(), diag.nodeUid);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(m_cook_result_mutex);
            // Last-wins: if the main thread hasn't drained the previous
            // result yet, overwrite it with this newer one.
            m_pending_cook_result = PendingCookResult{
                std::move(emitted),
                std::move(timings),
            };
        }
    }
}

// ── Render worker thread ──────────────────────────────────────────────
// Runs the rasterizer off the main thread so the UI never blocks on a
// GPU fence wait. render_tick on the main thread posts a snapshot (camera +
// shared_ptr to compiled scene) here; this loop drains the latest and
// dispatches. The output image is single-buffered; correctness across the
// "worker writing while main thread presents" boundary relies on:
//   • Both threads serializing their command-buffer submissions through
//     vulkanQueueMutex (Rasterizer::render now releases that mutex around
//     its fence wait, so contention is microseconds).
//   • Vulkan's per-queue execution ordering — a present-blit submitted
//     after a raster on the same queue can't start before the raster
//     finishes, even if both submissions sit briefly on the CPU side.
void EditorServer::render_thread_main() {
    for (;;) {
        RenderRequest request;
        {
            std::unique_lock<std::mutex> lk(m_render_mutex);
            m_render_cv.wait(lk, [this] {
                return m_render_thread_should_exit ||
                       m_pending_render_request.has_value();
            });
            if (m_render_thread_should_exit) return;
            request = std::move(*m_pending_render_request);
            m_pending_render_request.reset();
        }

        if (!request.scene || !m_engine) continue;

        try {
            const double ms = m_engine->render_rasterizer_with(
                *request.scene, request.camera);
            m_worker_raster_ms.store(ms, std::memory_order_relaxed);
            m_render_frames_completed.fetch_add(1, std::memory_order_release);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[render worker] dispatch failed: %s\n", e.what());
            // Don't bump the completion counter — main thread will keep
            // showing the previously-presented frame until we recover.
        }
    }
}

void EditorServer::stop_render_thread() {
    {
        std::lock_guard<std::mutex> lk(m_render_mutex);
        m_render_thread_should_exit = true;
    }
    m_render_cv.notify_one();
    if (m_render_thread.joinable()) m_render_thread.join();
}

// Called from the message thread under m_mutex. Hands a serialized graph
// (+ playhead time) to the worker. Returns immediately; the cook completes
// asynchronously and gets applied on the next render_tick.
void EditorServer::post_cook_request(std::string graph_json, double time) {
    // Cook the DOP graph forward to this frame and snapshot the current
    // sim state for every dop_import SOP. The worker thread re-stamps
    // these into its private graph clone before cook(), so the serialized
    // JSON doesn't have to carry geometry payloads.
    auto stamps = collect_dop_stamps(time);
    {
        std::lock_guard<std::mutex> lk(m_cook_request_mutex);
        m_pending_cook_request = CookRequest{
            std::move(graph_json),
            time,
            std::move(stamps),
        };
    }
    m_cook_request_cv.notify_one();
    // Tell the frontend the cook is in flight so it can surface a loading
    // status. The matching busy:false fires from drain_cook_result after
    // apply_emitted lands; if multiple requests coalesce (latest-wins), the
    // extra busy:true broadcasts are harmless — they just re-affirm state.
    if (m_broadcast) {
        m_broadcast(R"({"event":"cook_status","busy":true})");
    }
}

// Called from render_tick on the main thread (already holds m_mutex).
// Applies any cook result the worker has produced since the previous tick.
void EditorServer::drain_cook_result() {
    std::optional<PendingCookResult> result;
    {
        std::lock_guard<std::mutex> lk(m_cook_result_mutex);
        if (!m_pending_cook_result) return;
        result = std::move(m_pending_cook_result);
        m_pending_cook_result.reset();
    }
    apply_emitted(std::move(result->emitted));

    // Broadcast per-node timings so the profiler tab can refresh. Doing it
    // here (rather than in apply_emitted) keeps the profiler decoupled
    // from scene rebuild side-effects — it just needs the latest "what
    // did the worker do?" snapshot.
    if (m_broadcast && !result->timings.empty()) {
        json arr = json::array();
        double total_ms = 0.0;
        for (const auto& nct : result->timings) {
            arr.push_back({
                {"node_uid",        static_cast<uint64_t>(nct.nodeUid)},
                {"parent_node_uid", static_cast<uint64_t>(nct.parentNodeUid)},
                {"kind",            nct.kind},
                {"name",            nct.name},
                {"ms",              nct.ms},
            });
            total_ms += nct.ms;
        }
        json msg = {
            {"event",   "cook_timings"},
            {"total_ms", total_ms},
            {"rows",    std::move(arr)},
        };
        m_broadcast(msg.dump());
    }

    // Clear the loading status. If another request is already queued behind
    // this one, the next post_cook_request will re-broadcast busy:true.
    if (m_broadcast) {
        m_broadcast(R"({"event":"cook_status","busy":false})");
    }

    // Frame-locked playback: the cook completion just delivered frame N's
    // state. Advance the playhead by exactly 1/fps and post the next
    // cook so the chain keeps moving — wall-clock dt isn't driving it
    // in this mode, so without this step playback would stop after the
    // very first frame.
    //
    // We skip the kick if there's nothing for the cook to do (no
    // animated VOP promotion + no DOP imports — i.e. the graph is
    // static) so static scenes don't busy-loop the cook worker.
    if (m_timeline.frame_locked && m_timeline.playing &&
        !m_last_pushed_graph_json.empty() &&
        (m_has_animated_vop_promotions || m_has_dop_imports))
    {
        const double stride = 1.0 / std::max(m_timeline.fps, 1e-6);
        if (advance_playhead_by(stride)) {
            // Mark the timeline dirty so the next render_tick re-runs
            // animation overrides at the new time. The cook request is
            // posted here directly (not via render_tick) so the worker
            // stays continuously busy.
            m_timeline_dirty = true;
            post_cook_request(m_last_pushed_graph_json, m_timeline.current_time);
            // Broadcast the new playhead position so the dopesheet UI
            // tracks frame-locked playback the same way it tracks
            // wall-clock playback (timeline_tick in render_tick fires
            // at 30 Hz but only when m_timeline.playing && dt rolled
            // it forward, which doesn't happen here).
            if (m_broadcast) {
                json msg = {
                    {"event", "timeline_tick"},
                    {"time",  m_timeline.current_time},
                    {"playing", m_timeline.playing},
                };
                m_broadcast(msg.dump());
            }
        }
    }
}

// Per-frame offline render. Runs on m_export_thread; takes m_mutex for the
// cook + render so it doesn't race with command-handler scene mutations or
// the cook worker's apply_emitted(). render_tick() early-returns while we
// hold m_export_in_progress, which prevents the live viewport from also
// touching the path tracer.
void EditorServer::export_video_loop(VideoExportRequest req) {
    auto broadcast_event = [this](const json& msg) {
        if (m_broadcast) m_broadcast(msg.dump());
    };

    const int total = req.frame_end - req.frame_start + 1;
    if (total <= 0 || req.fps <= 0.0 || req.samples_per_frame <= 0) {
        broadcast_event({{"event", "video_export_error"},
                         {"message", "invalid range / fps / samples"}});
        m_export_in_progress.store(false);
        return;
    }

    // Snapshot the playhead so the user's timeline isn't permanently shifted
    // by an export. Restored on exit (success, error, or cancel).
    double saved_time = 0.0;
    bool saved_time_known = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        saved_time = m_timeline.current_time;
        saved_time_known = true;
    }

    // Snapshot resolutions and camera aspect so we can restore them on exit.
    // If the user passed explicit width/height, resize the path tracer to
    // that — leaving the rasterizer (and hence the live viewport overlay) at
    // whatever it was. set_resolutions() recreates the PT and the next
    // cook_and_apply() call will recompile against it.
    uint32_t width = 0, height = 0;
    uint32_t saved_raster_w = 0, saved_raster_h = 0;
    uint32_t saved_pt_w = 0, saved_pt_h = 0;
    float saved_aspect = 1.0f;
    bool resolution_changed = false;
    uint32_t saved_max_bounces = 0;
    bool bounces_changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_engine || !m_engine->path_tracer_ready() ||
            !m_engine->compiled_scene_ready() || !m_engine->scene().hasCamera()) {
            broadcast_event({{"event", "video_export_error"},
                             {"message", "engine not ready (open a scene first)"}});
            m_export_in_progress.store(false);
            return;
        }
        const auto [r_w, r_h] = m_engine->resolution();
        const auto [p_w, p_h] = m_engine->pt_resolution();
        saved_raster_w = r_w;
        saved_raster_h = r_h;
        saved_pt_w = p_w;
        saved_pt_h = p_h;
        saved_aspect = m_engine->scene().camera().aspectRatio();

        if (req.width > 0 && req.height > 0) {
            width = static_cast<uint32_t>(req.width);
            height = static_cast<uint32_t>(req.height);
            m_engine->set_resolutions(saved_raster_w, saved_raster_h, width, height);
            tracey::Camera cam = m_engine->scene().camera();
            cam.setAspectRatio(static_cast<float>(width) /
                               static_cast<float>(height));
            m_engine->scene().setCamera(cam);
            resolution_changed = (width != saved_pt_w || height != saved_pt_h);
        } else {
            width = saved_pt_w;
            height = saved_pt_h;
        }

        saved_max_bounces = m_engine->max_bounces();
        if (req.max_bounces > 0 &&
            static_cast<uint32_t>(req.max_bounces) != saved_max_bounces) {
            m_engine->set_max_bounces(static_cast<uint32_t>(req.max_bounces));
            bounces_changed = true;
        }
    }
    if (width == 0 || height == 0) {
        broadcast_event({{"event", "video_export_error"},
                         {"message", "viewport has zero size"}});
        m_export_in_progress.store(false);
        return;
    }

    auto restore_engine_state = [&]() {
        if (!resolution_changed && !bounces_changed) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (resolution_changed) {
            m_engine->set_resolutions(saved_raster_w, saved_raster_h,
                                      saved_pt_w, saved_pt_h);
            if (m_engine->scene().hasCamera()) {
                tracey::Camera cam = m_engine->scene().camera();
                cam.setAspectRatio(saved_aspect);
                m_engine->scene().setCamera(cam);
            }
        }
        if (bounces_changed) {
            m_engine->set_max_bounces(saved_max_bounces);
        }
        m_clear_next_frame = true;
    };

    VideoExporter exporter;
    const auto codec = (req.codec == "prores")
        ? VideoExporter::Codec::ProRes422
        : VideoExporter::Codec::H264;
    if (!exporter.begin(req.path, width, height,
                        static_cast<uint32_t>(std::lround(req.fps)), codec)) {
        broadcast_event({{"event", "video_export_error"},
                         {"message", exporter.last_error()}});
        restore_engine_state();
        m_export_in_progress.store(false);
        return;
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    bool success = true;

    // Detect once at the top of the export whether the graph is fully
    // static (no animated channels and no time-dependent VOPs). For a
    // static scene every per-frame cook produces byte-identical output —
    // so we cook+apply once before the first frame, then per frame only
    // render the requested samples. Skipping the cook for an N-second
    // export of a 20-mesh scene saves N*30 BLAS rebuilds.
    //
    // For animated scenes we fall back to the per-frame cook + apply
    // path, same as before. The proper per-actor BLAS cache lands next.
    bool scene_static = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        scene_static = !detect_any_animation();
    }
    bool cooked_once = false;

    for (int f = req.frame_start; f <= req.frame_end; ++f) {
        if (m_export_cancel.load()) break;

        // Frame numbering is 1-based; time at frame N is (N-1)/fps. Mirrors
        // stores/timeline.ts secondsForFrame().
        const double frame_time = (req.fps > 0.0) ? (f - 1) / req.fps : 0.0;
        const int frame_index = f - req.frame_start;  // 0-based for the writer

        // Step 1: seek + cook + accumulate samples — all under m_mutex.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_timeline.current_time = frame_time;
            // Skip the per-frame cook+apply on static scenes after the
            // first frame — the BLAS / scene state is already correct.
            if (!scene_static || !cooked_once) {
                cook_and_apply();
                cooked_once = true;
            }
            apply_animation_at(frame_time);

            for (int s = 0; s < req.samples_per_frame; ++s) {
                if (m_export_cancel.load()) break;
                const bool clear = (s == 0);
                m_engine->render_frame(clear);
            }
        }

        if (m_export_cancel.load()) break;

        // Step 2: pull the accumulated pixels back and append. We re-take the
        // lock to read the path tracer state coherently, then drop it across
        // the (potentially blocking) AVAssetWriter call so command handlers
        // can answer e.g. an export_video_cancel from the dialog.
        std::vector<uint8_t> pixels;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const size_t bytes = static_cast<size_t>(width) * height * 4;
            pixels.resize(bytes);
            m_engine->path_tracer()->readback(pixels.data());
        }

        if (!exporter.append_frame(pixels.data(),
                                   static_cast<uint32_t>(frame_index))) {
            broadcast_event({{"event", "video_export_error"},
                             {"message", exporter.last_error()}});
            success = false;
            break;
        }

        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        broadcast_event({{"event", "video_export_progress"},
                         {"frame", frame_index + 1},
                         {"total", total},
                         {"elapsed_ms", elapsed_ms}});
    }

    const bool cancelled = m_export_cancel.load();
    const bool finished_ok = exporter.finish(cancelled || !success);

    // Restore PT resolution + camera aspect for the live viewport before we
    // hand control back. Done before the playhead restore so the next
    // render_tick sees a consistent state.
    restore_engine_state();

    // Restore the user's playhead so the live viewport doesn't pop to the
    // export's last frame.
    if (saved_time_known) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_timeline.current_time = saved_time;
        m_timeline_dirty = true;
    }

    if (!success) {
        // Already broadcast video_export_error above.
    } else if (cancelled) {
        broadcast_event({{"event", "video_export_done"},
                         {"path", req.path},
                         {"cancelled", true}});
    } else if (!finished_ok) {
        broadcast_event({{"event", "video_export_error"},
                         {"message", exporter.last_error()}});
    } else {
        broadcast_event({{"event", "video_export_done"},
                         {"path", req.path},
                         {"cancelled", false}});
    }

    m_export_cancel.store(false);
    m_export_in_progress.store(false);
}

bool EditorServer::update_camera_from_input(double /*dt*/) {
    if (!m_window) return false;
    auto& input = m_window->input();

    if (!m_engine->scene().hasCamera()) {
        // Default camera if scene didn't ship one. Pulled up and back so the
        // ground grid is visible at a slight downward angle on first frame —
        // the previous (0,0,5) eye-level pose put the y=0 plane edge-on.
        // Rotation is set via lookAt so the very first render (before the
        // orbital state seeds itself in this same function) already frames
        // the origin properly.
        tracey::Camera cam;
        const glm::vec3 pos(0.0f, 4.0f, 8.0f);
        const glm::vec3 target(0.0f, 0.0f, 0.0f);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        cam.setPosition(pos);
        cam.setRotation(glm::quat_cast(glm::inverse(glm::lookAt(pos, target, up))));
        m_engine->scene().setCamera(cam);
    }
    tracey::Camera cam = m_engine->scene().camera();

    if (!m_orbit_initialized) {
        // Seed the orbital state from the existing camera. Pivot defaults to
        // world origin; distance is the camera's distance from it. Yaw/pitch
        // are derived from the camera's forward direction so the first frame
        // is visually identical to the input camera.
        glm::vec3 toCam = glm::vec3(cam.position());
        m_orbit_pivot_x = 0.0f;
        m_orbit_pivot_y = 0.0f;
        m_orbit_pivot_z = 0.0f;
        m_orbit_distance = std::max(0.1f, glm::length(toCam));
        glm::vec3 fwd = -glm::normalize(toCam.x == 0.f && toCam.y == 0.f && toCam.z == 0.f
                                        ? glm::vec3(0, 0, 1)
                                        : toCam);
        m_orbit_pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
        m_orbit_yaw = std::atan2(-fwd.x, -fwd.z);
        m_orbit_initialized = true;
    }

    bool changed = false;

    // Drag-to-navigate: LMB tumbles, MMB pans, RMB dollies — no modifier
    // key. The Space modifier was dropped because Space is reserved for
    // the timeline (play/pause), and the user wants camera navigation to
    // be the default mouse behaviour in the viewport rather than a
    // gated mode.
    constexpr float TUMBLE_SENS = 0.005f;  // radians per pixel
    constexpr float DOLLY_SENS  = 0.01f;   // log-units per pixel
    constexpr float WHEEL_SENS  = 0.05f;   // log-units per scroll tick

    if (input.mouse_dx != 0.0f || input.mouse_dy != 0.0f) {
        if (input.mouse_left) {
            m_orbit_yaw   -= input.mouse_dx * TUMBLE_SENS;
            m_orbit_pitch -= input.mouse_dy * TUMBLE_SENS;
            constexpr float kPitchLimit = 1.5707f - 0.01f;
            m_orbit_pitch = std::clamp(m_orbit_pitch, -kPitchLimit, kPitchLimit);
            changed = true;
        } else if (input.mouse_middle) {
            // Pan: scale by distance + fov so a fixed pixel delta moves the
            // pivot by the same screen-space amount regardless of zoom.
            const float vh = std::max(1.0f, static_cast<float>(m_viewport_pixel_h));
            const float worldPerPx = 2.0f * m_orbit_distance *
                std::tan(glm::radians(cam.fov() * 0.5f)) / vh;
            const glm::vec3 r = cam.right();
            const glm::vec3 u = cam.up();
            const glm::vec3 delta = -r * (input.mouse_dx * worldPerPx) +
                                     u * (input.mouse_dy * worldPerPx);
            m_orbit_pivot_x += delta.x;
            m_orbit_pivot_y += delta.y;
            m_orbit_pivot_z += delta.z;
            changed = true;
        } else if (input.mouse_right) {
            // Dolly: exponential so zooming stays smooth at any distance.
            m_orbit_distance *= std::exp(input.mouse_dy * DOLLY_SENS);
            m_orbit_distance = std::max(0.01f, m_orbit_distance);
            changed = true;
        }
    }
    input.mouse_dx = 0.0f;
    input.mouse_dy = 0.0f;

    if (input.scroll_dy != 0.0f) {
        m_orbit_distance *= std::exp(-input.scroll_dy * WHEEL_SENS);
        m_orbit_distance = std::max(0.01f, m_orbit_distance);
        changed = true;
    }
    input.scroll_dx = 0.0f;
    input.scroll_dy = 0.0f;

    if (changed) {
        // Compose yaw (around world Y) then pitch (around local X) and place
        // the camera on the orbit sphere centred at the pivot.
        glm::quat qyaw   = glm::angleAxis(m_orbit_yaw,   glm::vec3(0, 1, 0));
        glm::quat qpitch = glm::angleAxis(m_orbit_pitch, glm::vec3(1, 0, 0));
        glm::quat rotation = qyaw * qpitch;
        glm::vec3 forward = rotation * glm::vec3(0, 0, -1);
        glm::vec3 pivot{m_orbit_pivot_x, m_orbit_pivot_y, m_orbit_pivot_z};
        cam.setPosition(pivot - forward * m_orbit_distance);
        cam.setRotation(rotation);
        if (m_viewport_pixel_h > 0)
            cam.setAspectRatio(static_cast<float>(m_viewport_pixel_w) /
                               static_cast<float>(m_viewport_pixel_h));
        m_engine->scene().setCamera(cam);
    }
    return changed;
}

void EditorServer::render_tick() {
    // Pause the live viewport while the offline export worker has the engine.
    // The worker drives its own seek/cook/render loop and would otherwise
    // contend with us for the path tracer state.
    if (m_export_in_progress.load()) return;

    // try_lock instead of lock: render_tick fires from CVDisplayLink via
    // dispatch_async(main_queue). If a command handler holds m_mutex while
    // pumping the main run loop (e.g. NSSavePanel/NSOpenPanel runModal), the
    // dispatched tick would re-enter the same thread on the same std::mutex
    // and deadlock. Skipping the frame is cheap; the next vsync retries.
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;

    // Wall-clock dt — needed for both the camera fly-through and the
    // playhead. Computed up front so the playhead advances even before a
    // viewport renderer exists (useful for headless smoke tests).
    using clock = std::chrono::steady_clock;
    const auto tickStart = clock::now();
    const double now = std::chrono::duration<double>(tickStart.time_since_epoch()).count();
    const double dt = m_last_tick_time > 0.0 ? std::min(now - m_last_tick_time, 0.1) : 1.0 / 60.0;
    m_last_tick_time = now;

    // Phase timing helper. We can't measure `present_ms` until after the
    // present at the bottom of render_tick, so the broadcast below
    // publishes the *previous* tick's slice as a 1-frame-delayed
    // approximation. At 60 FPS that's ~16 ms older than the headline
    // FPS but well under the 250 ms broadcast cadence, so the user
    // can't perceive the delay; it just keeps the timing code linear.
    double rebuild_ms = 0.0;
    double raster_ms  = 0.0;
    double present_ms = 0.0;
    auto elapsedMs = [](clock::time_point start) {
        return std::chrono::duration<double, std::milli>(clock::now() - start).count();
    };

    // Apply any cook result the worker produced since the last tick. Cook
    // resets actor transforms to constants; apply_emitted sets
    // m_timeline_dirty so we re-evaluate animation overrides below.
    // Wrap in a try/catch: an exception escaping here would terminate the
    // process because the tick runs on the main thread (CVDisplayLink →
    // dispatch_async(main_queue)). The render-pass try/catch further down
    // doesn't cover this region.
    {
        const auto t0 = clock::now();
        try {
            drain_cook_result();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[render_tick] drain_cook_result failed: %s\n", e.what());
        }
        rebuild_ms = elapsedMs(t0);
    }

    // Advance the playhead during playback. Seeks via timeline_set_playhead
    // already updated current_time + set m_timeline_dirty, so we only need to
    // step it forward here.
    const bool playhead_moved = advance_playhead(dt);

    // Re-evaluate animated overrides whenever the playhead moved or a key
    // was edited / cook completed.
    const bool time_changed = playhead_moved || m_timeline_dirty;
    if (time_changed) {
        apply_animation_at(m_timeline.current_time);
        m_timeline_dirty = false;

        // Auto re-cook when the graph has animated VOP promotions: the
        // override path can't reach VOP-side knobs (only actor transforms),
        // so we re-cook the cached root JSON with the new playhead time.
        // Latest-wins in the worker keeps this cheap during rapid scrub.
        // Also fire when the graph has dop_import nodes — those need a
        // fresh sim-state stamp every frame (post_cook_request collects
        // the stamps automatically via collect_dop_stamps). The two
        // branches share the post; combine into one condition.
        const bool need_recook = (m_has_animated_vop_promotions || m_has_dop_imports)
                                 && !m_last_pushed_graph_json.empty();
        if (need_recook) {
            post_cook_request(m_last_pushed_graph_json, m_timeline.current_time);
        }
    }

    // Rate-limited timeline broadcast (~30 Hz) so the frontend playbar
    // follows the playhead without flooding the message bus.
    if (m_timeline.playing && m_broadcast &&
        (now - m_last_timeline_broadcast) > (1.0 / 30.0)) {
        m_last_timeline_broadcast = now;
        json msg = {
            {"event", "timeline_tick"},
            {"time", m_timeline.current_time},
            {"playing", m_timeline.playing},
        };
        m_broadcast(msg.dump());
    }

    if (!m_viewport_active || !m_window) return;
    if (!m_engine->path_tracer_ready() || !m_engine->rasterizer_ready() ||
        !m_engine->compiled_scene_ready()) return;

    ensure_viewport_renderer(m_window->viewport_pixel_width(),
                             m_window->viewport_pixel_height());
    if (!m_viewport) return;

    // Suppress camera orbit/pan/dolly while the JS modal grab owns the
    // mouse; broadcast pointer state instead so the grab math can run.
    if (m_viewport_grab_active) {
        if (m_window && m_broadcast) {
            auto& input = m_window->input();
            const bool changed =
                input.mouse_x != m_last_broadcast_mouse_x ||
                input.mouse_y != m_last_broadcast_mouse_y ||
                input.mouse_left  != m_last_broadcast_mouse_left ||
                input.mouse_right != m_last_broadcast_mouse_right;
            if (changed) {
                m_last_broadcast_mouse_x     = input.mouse_x;
                m_last_broadcast_mouse_y     = input.mouse_y;
                m_last_broadcast_mouse_left  = input.mouse_left;
                m_last_broadcast_mouse_right = input.mouse_right;
                json msg = {
                    {"event", "viewport_pointer"},
                    {"x", input.mouse_x},
                    {"y", input.mouse_y},
                    {"left",  input.mouse_left},
                    {"right", input.mouse_right},
                };
                m_broadcast(msg.dump());
            }
            // Drain the deltas so the camera doesn't snap when the grab
            // ends (otherwise the accumulated dx/dy from the drag would
            // fire on the first tick after the JS sends grab_active=false).
            input.mouse_dx = 0.0f;
            input.mouse_dy = 0.0f;
        }
    } else {
        if (update_camera_from_input(dt)) m_clear_next_frame = true;
    }

    const bool has_geometry = m_engine->has_renderable_geometry();
    try {
        // Only consume the pending clear when the PT actually dispatches —
        // otherwise a clear request raised while preview is off (camera
        // move, scene edit) would be silently lost. set_pt_preview's
        // off→on handler also raises this flag, so the very first frame
        // after enabling always starts a fresh accumulation.
        const bool clear = m_pt_preview_enabled && m_clear_next_frame;
        if (m_pt_preview_enabled) m_clear_next_frame = false;
        // Rasterizer is offloaded to a worker thread. Snapshot the scene
        // + camera under m_mutex (we already hold it via the tick's
        // try_lock above), then hand off to the worker. The worker writes
        // into the rasterizer's single output image; we present that image
        // below once it has produced at least one completed frame.
        //
        // Latest-wins: if the worker hasn't drained the previous request,
        // we overwrite it. That matches the live-viewport expectation —
        // we always want the freshest snapshot, not a backlog of stale
        // ones.
        if (m_engine->compiled_scene_ready() && m_engine->scene().hasCamera())
        {
            RenderRequest req;
            req.scene = m_engine->compiled_scene_snapshot();
            req.camera = m_engine->scene().camera();
            {
                std::lock_guard<std::mutex> rlk(m_render_mutex);
                m_pending_render_request = std::move(req);
            }
            m_render_cv.notify_one();
        }
        // The worker reports its measured ms back; we just read the
        // most recent value for the profiler bucket.
        raster_ms = m_worker_raster_ms.load(std::memory_order_relaxed);
        // Expensive path tracer pass: needs a BVH, so it only runs once the
        // scene has at least one instance. Accumulates into the inset rect,
        // one sample per tick, until max_samples is reached.
        const bool at_cap = !clear &&
            m_engine->current_samples() >= m_engine->max_samples();
        // The path tracer dispatch is the most expensive thing in
        // render_tick — skip it entirely when the inset preview is off.
        // We still run the rasterizer above so the viewport stays live.
        if (m_pt_preview_enabled && has_geometry && !at_cap) {
            // want_pixels=false: the live viewport composites
            // path_tracer()->outputImage() straight off the GPU through
            // ViewportRenderer::present_composite. Pulling the framebuffer
            // back to the CPU here would just discard it after grabbing
            // width/height/render_time_ms — and the elided
            // vkCmdCopyImageToBuffer + mapForReading shave the unmeasured
            // chunk out of the per-tick budget.
            auto result = m_engine->render_frame(clear, /*want_pixels=*/false);
            m_last_render_width = result.width;
            m_last_render_height = result.height;
            m_last_render_time_ms = result.render_time_ms;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewport] render failed: %s\n", e.what());
        return;
    }

    auto* tracer = m_engine->path_tracer();
    auto* raster = m_engine->rasterizer();
    if (tracer && raster) {
        auto* raster_output = raster->outputImage();
        // Worker may not have produced its first raster frame yet (first
        // tick after launch, before the worker has drained any request).
        // Blitting the rasterizer's output image without a completed
        // dispatch would show uninitialised content; gate everything that
        // reads `raster_output` on this counter. The PT-fullscreen branch
        // doesn't need raster at all, so it stays reachable.
        const bool raster_ready =
            m_render_frames_completed.load(std::memory_order_acquire) > 0;
        const auto t0 = clock::now();
        bool presented = false;
        if (m_pt_preview_enabled && has_geometry && m_pt_fullscreen) {
            // Render workspace: PT replaces the rasterizer entirely.
            // No raster output needed.
            if (auto* pt_output = tracer->outputImage()) {
                if (!m_viewport->present(pt_output)) {
                    m_viewport_pixel_w = 0;
                    m_viewport_pixel_h = 0;
                }
                presented = true;
            }
        } else if (raster_output && raster_ready) {
            if (!has_geometry || !m_pt_preview_enabled) {
                // Raster-only present. Most common path during scene
                // editing; the rasterizer-only branch skips the composite
                // sampler bind + inset blit in ViewportRenderer.
                if (!m_viewport->present(raster_output)) {
                    m_viewport_pixel_w = 0;
                    m_viewport_pixel_h = 0;
                }
                presented = true;
            } else if (auto* pt_output = tracer->outputImage()) {
                // PiP composite: rasterizer fills the viewport, PT
                // accumulator overlays in the top-right inset.
                const InsetRect r = compute_inset_rect(m_viewport_pixel_w, m_viewport_pixel_h);
                if (!m_viewport->present_composite(raster_output, pt_output,
                                                   r.x, r.y, r.w, r.h)) {
                    m_viewport_pixel_w = 0;
                    m_viewport_pixel_h = 0;
                }
                presented = true;
            }
        }
        if (presented) present_ms = elapsedMs(t0);
    }

    const double tick_ms = elapsedMs(tickStart);

    // EMA-smooth the buckets so the profiler readout doesn't twitch
    // on per-tick jitter. The 0.9/0.1 weighting matches m_smoothed_dt:
    // ~10-tick half-life, which at the broadcast cadence below feels
    // responsive without being noisy.
    auto ema = [](double prev, double curr) {
        return prev > 1e-9 ? prev * 0.9 + curr * 0.1 : curr;
    };
    m_smoothed_tick_ms    = ema(m_smoothed_tick_ms,    tick_ms);
    m_smoothed_rebuild_ms = ema(m_smoothed_rebuild_ms, rebuild_ms);
    m_smoothed_raster_ms  = ema(m_smoothed_raster_ms,  raster_ms);
    m_smoothed_present_ms = ema(m_smoothed_present_ms, present_ms);

    // Throttled render-stats broadcast for the profiler tab. 250 ms
    // cadence is fast enough to feel responsive and well under the
    // message-bus overhead the timeline_tick broadcasts establish.
    if (dt > 0.0) m_smoothed_dt = m_smoothed_dt * 0.9 + dt * 0.1;
    if (m_broadcast && now - m_last_render_stats_broadcast > 0.25) {
        m_last_render_stats_broadcast = now;
        const double fps = (m_smoothed_dt > 1e-6) ? (1.0 / m_smoothed_dt) : 0.0;
        json msg = {
            {"event",           "render_stats"},
            {"fps",             fps},
            {"render_time_ms",  m_last_render_time_ms},
            {"tick_ms",         m_smoothed_tick_ms},
            {"rebuild_ms",      m_smoothed_rebuild_ms},
            {"raster_ms",       m_smoothed_raster_ms},
            {"present_ms",      m_smoothed_present_ms},
            {"triangles",       static_cast<uint64_t>(m_engine->total_triangles())},
            {"instances",       static_cast<uint64_t>(m_engine->total_instances())},
            {"bvh_nodes",       static_cast<uint64_t>(m_engine->total_bvh_nodes())},
            {"samples",         m_engine->current_samples()},
            {"max_samples",     m_engine->max_samples()},
        };
        m_broadcast(msg.dump());
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

    // Native modal file dialogs ([NSOpenPanel runModal] etc.) pump the main
    // run loop while open, which means other JS commands queued on the same
    // thread try to re-acquire m_mutex and deadlock the main thread until
    // WebKit's deferral block times out (beachball). Dialogs don't touch
    // shared engine state, so handle them BEFORE taking the lock.
    if (cmd == "open_file_dialog" || cmd == "save_file_dialog" ||
        cmd == "open_folder_dialog") {
        if (!m_window) return err_response("No window for dialog");
        try {
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
            std::string path;
            if (cmd == "open_file_dialog") {
                const auto title = req.value("title", std::string{"Open"});
                path = m_window->open_file_dialog(title.c_str(), filters);
            } else if (cmd == "save_file_dialog") {
                const auto title = req.value("title", std::string{"Save"});
                const auto default_name = req.value("default_name", std::string{});
                path = m_window->save_file_dialog(title.c_str(),
                                                  default_name.c_str(),
                                                  filters);
            } else {
                const auto title = req.value("title", std::string{"Open Folder"});
                path = m_window->open_folder_dialog(title.c_str());
            }
            return ok_response(path.empty() ? json(nullptr) : json(path));
        } catch (const std::exception& e) {
            return err_response(std::string{"dialog error: "} + e.what());
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    try {
        // ── Scene management ──
        if (cmd == "create_actor") {
            const auto name = req.value("name", std::string{});
            auto* actor = m_engine->scene().createActor();
            if (!name.empty()) actor->setName(name);
            return ok_response(actor->getUid());
        }
        if (cmd == "create_light") {
            // Manual / editor-authored light. Distinct from SOP-emitted
            // lights (which travel via EmittedActor with isLight=true) in
            // that we never associate this actor with a source SOP node,
            // so a cook can't reset its parameters. The actor still
            // round-trips through save/load via actor_to_json above.
            const std::string typeStr = req.value("type", std::string("dome"));
            tracey::Light light;
            tracey::Transform xform;
            std::string defaultName;
            if (typeStr == "dome") {
                light.type = tracey::LightType::Dome;
                defaultName = "Dome";
                // Dome is transform-independent — leaving the actor at
                // origin keeps the hierarchy tidy.
            } else if (typeStr == "sun" || typeStr == "distant") {
                light.type = tracey::LightType::Distant;
                defaultName = "Sun";
                // Default sun rotation matches the constants the pre-light
                // shader used (normalize(0.4, 0.8, 0.3)). We pre-rotate
                // the actor so the local -Z direction lands on that vector.
                const tracey::Vec3 fwd = glm::normalize(tracey::Vec3(-0.4f, -0.8f, -0.3f));
                const tracey::Quaternion q = glm::rotation(tracey::Vec3(0.0f, 0.0f, -1.0f), fwd);
                xform.setRotation(q);
            } else if (typeStr == "area") {
                light.type = tracey::LightType::Area;
                defaultName = "Area";
                xform.setPosition(tracey::Vec3(0.0f, 3.0f, 0.0f));
            } else {
                light.type = tracey::LightType::Point;
                defaultName = "Point";
                xform.setPosition(tracey::Vec3(0.0f, 2.0f, 0.0f));
            }
            const std::string name = req.value("name", defaultName);

            auto* actor = m_engine->scene().createActor();
            actor->setName(name);
            actor->setTransform(xform);
            actor->setLight(light);

            // Re-compile so the renderer's lightBuffer picks up the new
            // entry on the next frame. Cheap relative to a SOP cook —
            // light gather is the last pass and walks a tiny list.
            if (m_engine->path_tracer_ready()) m_engine->compile_scene();
            m_clear_next_frame = true;
            if (m_broadcast) m_broadcast(R"({"event":"scene_changed"})");
            return ok_response(actor->getUid());
        }
        if (cmd == "set_light_params") {
            // Patch handler: the frontend sends only the keys it changed,
            // so each field is `value(key, current)` so missing keys
            // pass through unchanged. Triggers a recompile because both
            // the rasterizer's SSBO bind AND the PT's NEE buffer need
            // the updated bytes.
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            auto* a = m_engine->scene().getActor(id);
            if (!a || !a->hasLight()) return ok_response(false);
            tracey::Light light = *a->light();

            if (req.contains("type")) {
                light.type = static_cast<tracey::LightType>(
                    req.at("type").get<int>());
            }
            auto readVec3 = [&](const char* key, tracey::Vec3& v) {
                if (!req.contains(key)) return;
                const auto& j = req.at(key);
                v.x = j.at("x").get<float>();
                v.y = j.at("y").get<float>();
                v.z = j.at("z").get<float>();
            };
            readVec3("color",         light.color);
            readVec3("sky_color",     light.skyColor);
            readVec3("horizon_color", light.horizonColor);
            readVec3("ground_color",  light.groundColor);
            if (req.contains("intensity")) light.intensity = req.at("intensity").get<float>();
            if (req.contains("radius"))    light.radius    = req.at("radius").get<float>();
            if (req.contains("size")) {
                const auto& j = req.at("size");
                light.size.x = j.at("x").get<float>();
                light.size.y = j.at("y").get<float>();
            }
            if (req.contains("hdri_path")) light.hdriPath = req.at("hdri_path").get<std::string>();
            a->setLight(light);

            if (m_engine->path_tracer_ready()) m_engine->compile_scene();
            m_clear_next_frame = true;
            if (m_broadcast) m_broadcast(R"({"event":"scene_changed"})");
            return ok_response(true);
        }
        if (cmd == "get_all_actors") {
            // Invert the SOP-uid → actor-uid map once, so each actor can be
            // tagged with its source object_output node (used by the frontend
            // to target keyframe edits at the right SOP parameter).
            std::unordered_map<uint64_t, size_t> actor_to_sop;
            actor_to_sop.reserve(m_sop_node_to_actor.size());
            for (const auto& [outputUid, actorUid] : m_sop_node_to_actor) {
                actor_to_sop[actorUid] = outputUid;
            }
            // Hierarchy roll-up: an `instance` SOP emits N renderer-side
            // Actors (one per template point) but the user only wants to
            // see ONE row in the hierarchy per instance SOP. Drop the
            // instanceIndex > 0 actors; their primary (instanceIndex == 0)
            // is enough to represent the whole group, and the renderer
            // TLAS is unaffected by what we choose to expose to JS.
            // Without this, a 120-particle sim ships 120 actor JSON rows
            // per cook to the frontend at ~60 cooks/sec, which Solid's
            // tree-diff + the IPC round-trip can't keep up with.
            // make_actor_key layout: low 24 bits = instanceIndex, high
            // 40 bits = sourceNodeUid (matches make_actor_key in this file).
            std::unordered_set<uint64_t> secondary_instances;
            for (const auto& [compositeKey, actorUid] : m_emitted_actor_to_actor) {
                const uint32_t instanceIndex =
                    static_cast<uint32_t>(compositeKey & 0xFFFFFFu);
                if (instanceIndex > 0) secondary_instances.insert(actorUid);
            }
            json arr = json::array();
            for (const auto* a : m_engine->scene().actors()) {
                if (secondary_instances.contains(a->getUid())) continue;
                const auto it = actor_to_sop.find(a->getUid());
                arr.push_back(actor_to_json(*a, it != actor_to_sop.end() ? it->second : 0));
            }
            return ok_response(arr);
        }
        if (cmd == "get_actor") {
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            const auto* a = m_engine->scene().getActor(id);
            if (!a) return ok_response_null();
            size_t sourceSop = 0;
            for (const auto& [outputUid, actorUid] : m_sop_node_to_actor) {
                if (actorUid == id) { sourceSop = outputUid; break; }
            }
            return ok_response(actor_to_json(*a, sourceSop));
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
                for (const auto& [outputUid, actorUid] : m_sop_node_to_actor) {
                    if (actorUid != id) continue;
                    auto* node = findNodeRecursive(m_sop_graph.get(),outputUid);
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
        if (cmd == "set_actor_rotation_euler") {
            // Sibling to set_actor_transform but expressed in euler-degrees,
            // mirroring the SOP node's `rotate_euler_deg` storage so we
            // never have to do the lossy quat → euler conversion. The
            // inspector's rotation row uses this directly.
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            auto* a = m_engine->scene().getActor(id);
            if (!a) return ok_response(false);
            const auto eulerJson = req.at("euler_deg");
            const tracey::Vec3 deg = vec3_from_json(eulerJson);

            constexpr float kDeg2Rad = 3.1415926535f / 180.0f;
            const tracey::Vec3 rad = deg * kDeg2Rad;
            glm::quat qx = glm::angleAxis(rad.x, glm::vec3(1, 0, 0));
            glm::quat qy = glm::angleAxis(rad.y, glm::vec3(0, 1, 0));
            glm::quat qz = glm::angleAxis(rad.z, glm::vec3(0, 0, 1));
            tracey::Transform xf = a->transform();
            xf.setRotation(qz * qy * qx);
            a->setTransform(xf);

            // Write back to the SOP node so the edit survives the next cook.
            // Same shape as set_actor_transform's writeback for translate/scale.
            bool sopMutated = false;
            if (m_sop_graph) {
                for (const auto& [outputUid, actorUid] : m_sop_node_to_actor) {
                    if (actorUid != id) continue;
                    auto* node = findNodeRecursive(m_sop_graph.get(), outputUid);
                    if (!node) break;
                    node->setParamVec3("rotate_euler_deg", deg);
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
        if (cmd == "set_actor_visible") {
            // Per-actor display flag. The flag lives on the Actor in the live
            // scene AND in m_sop_node_visible (keyed by the actor's source SOP
            // node uid) so it survives a re-cook. Toggling triggers a recompile
            // so the path tracer's TLAS drops / regains the actor's instances.
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            const bool vis = req.at("visible").get<bool>();
            auto* a = m_engine->scene().getActor(id);
            if (!a) return ok_response(false);
            a->setVisible(vis);
            for (const auto& [sopUid, actorUid] : m_sop_node_to_actor) {
                if (actorUid == id) { m_sop_node_visible[sopUid] = vis; break; }
            }
            if (m_engine->path_tracer_ready()) m_engine->compile_scene();
            m_clear_next_frame = true;
            if (m_broadcast) m_broadcast(R"({"event":"scene_changed"})");
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
        if (cmd == "select_actor") {
            // Frontend hands us the active selection so the orbital camera
            // pivot tracks it. `actor_id` may be null on deselect.
            const auto& idField = req.at("actor_id");
            if (idField.is_null()) {
                m_selected_actor_id.reset();
            } else {
                const uint64_t id = idField.get<uint64_t>();
                m_selected_actor_id = id;
                // Find the actor's world position via flatten() so any parent
                // chain transforms apply, then snap the pivot to it. We keep
                // current yaw/pitch/distance so the camera "swings" to the new
                // pivot rather than teleporting the user.
                for (const auto& node : m_engine->scene().flatten()) {
                    if (!node.actor || node.actor->getUid() != id) continue;
                    const glm::vec4 origin = node.worldTransform * glm::vec4(0, 0, 0, 1);
                    m_orbit_pivot_x = origin.x;
                    m_orbit_pivot_y = origin.y;
                    m_orbit_pivot_z = origin.z;
                    // Recompose the camera so the pivot change is applied
                    // immediately, not on the next mouse delta.
                    if (m_engine->scene().hasCamera() && m_orbit_initialized) {
                        tracey::Camera cam = m_engine->scene().camera();
                        glm::quat qyaw   = glm::angleAxis(m_orbit_yaw,   glm::vec3(0, 1, 0));
                        glm::quat qpitch = glm::angleAxis(m_orbit_pitch, glm::vec3(1, 0, 0));
                        glm::quat rotation = qyaw * qpitch;
                        glm::vec3 forward = rotation * glm::vec3(0, 0, -1);
                        glm::vec3 pivot{m_orbit_pivot_x, m_orbit_pivot_y, m_orbit_pivot_z};
                        cam.setPosition(pivot - forward * m_orbit_distance);
                        cam.setRotation(rotation);
                        m_engine->scene().setCamera(cam);
                        m_clear_next_frame = true;
                    }
                    break;
                }
            }
            return ok_response_null();
        }
        if (cmd == "set_camera_view") {
            // Snap the orbital camera to a named preset (top, front, side,
            // perspective, …). Pivot + distance are preserved so pressing a
            // preset reframes whatever the user is currently focused on.
            const auto view = req.at("view").get<std::string>();
            constexpr float kPitchLimit = 1.5707f - 0.01f;
            constexpr float kHalfPi = 1.5707963f;
            if (view == "top") {
                m_orbit_yaw   = 0.0f;
                m_orbit_pitch = -kPitchLimit;       // looking straight down
            } else if (view == "bottom") {
                m_orbit_yaw   = 0.0f;
                m_orbit_pitch =  kPitchLimit;       // looking straight up
            } else if (view == "front") {
                m_orbit_yaw   = 0.0f;
                m_orbit_pitch = 0.0f;               // camera on +Z, looking -Z
            } else if (view == "back") {
                m_orbit_yaw   = kHalfPi * 2.0f;     // camera on -Z, looking +Z
                m_orbit_pitch = 0.0f;
            } else if (view == "right" || view == "side") {
                m_orbit_yaw   = kHalfPi;            // camera on +X, looking -X
                m_orbit_pitch = 0.0f;
            } else if (view == "left") {
                m_orbit_yaw   = -kHalfPi;           // camera on -X, looking +X
                m_orbit_pitch = 0.0f;
            } else if (view == "persp" || view == "perspective") {
                m_orbit_yaw   = -0.7854f;           // -45°: 3/4 view onto origin
                m_orbit_pitch = -0.4636f;           // ~-26.6° downward
            } else {
                return err_response("Unknown camera view: " + view);
            }
            // If the orbital state was never primed (e.g. no user input yet
            // since launch), seed pivot at origin + a sensible default
            // distance so the preset actually frames something.
            if (!m_orbit_initialized) {
                m_orbit_pivot_x = m_orbit_pivot_y = m_orbit_pivot_z = 0.0f;
                if (m_orbit_distance <= 0.0f) m_orbit_distance = 8.0f;
                m_orbit_initialized = true;
            }
            if (m_engine->scene().hasCamera()) {
                tracey::Camera cam = m_engine->scene().camera();
                glm::quat qyaw   = glm::angleAxis(m_orbit_yaw,   glm::vec3(0, 1, 0));
                glm::quat qpitch = glm::angleAxis(m_orbit_pitch, glm::vec3(1, 0, 0));
                glm::quat rotation = qyaw * qpitch;
                glm::vec3 forward = rotation * glm::vec3(0, 0, -1);
                glm::vec3 pivot{m_orbit_pivot_x, m_orbit_pivot_y, m_orbit_pivot_z};
                cam.setPosition(pivot - forward * m_orbit_distance);
                cam.setRotation(rotation);
                m_engine->scene().setCamera(cam);
                m_clear_next_frame = true;
            }
            return ok_response_null();
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
            const InsetRect r = compute_inset_rect(w, h);
            // set_resolutions skips work if neither raster nor PT size actually
            // changed; only invalidate the accumulator when it did. The
            // frontend reports rect changes on every layout shift, so clearing
            // unconditionally here would prevent any path-tracer accumulation.
            const auto [oldR_w, oldR_h] = m_engine->resolution();
            const auto [oldP_w, oldP_h] = m_engine->pt_resolution();
            const bool sizes_changed =
                w != oldR_w || h != oldR_h || r.w != oldP_w || r.h != oldP_h;
            m_engine->set_resolutions(w, h, r.w, r.h);
            if (sizes_changed) m_clear_next_frame = true;
            return ok_response_null();
        }
        if (cmd == "get_max_samples") {
            return ok_response(m_engine->max_samples());
        }
        if (cmd == "set_max_samples") {
            m_engine->set_max_samples(req.at("samples").get<uint32_t>());
            // Don't reset accumulation: raising the cap continues from the
            // current sample count; lowering it just stops further dispatch.
            return ok_response_null();
        }
        if (cmd == "get_current_samples") {
            return ok_response(m_engine->current_samples());
        }
        if (cmd == "get_max_bounces") {
            return ok_response(m_engine->max_bounces());
        }
        if (cmd == "set_max_bounces") {
            m_engine->set_max_bounces(req.at("bounces").get<uint32_t>());
            return ok_response_null();
        }
        if (cmd == "get_show_points") {
            return ok_response(m_engine->show_points());
        }
        if (cmd == "set_show_points") {
            m_engine->set_show_points(req.at("value").get<bool>());
            // Don't reset path-tracer accumulation: points are rasterizer-only.
            return ok_response_null();
        }
        if (cmd == "get_show_edges") {
            return ok_response(m_engine->show_edges());
        }
        if (cmd == "set_show_edges") {
            m_engine->set_show_edges(req.at("value").get<bool>());
            // Wireframe is rasterizer-only; PT inset is unaffected.
            return ok_response_null();
        }
        if (cmd == "get_show_ground") {
            return ok_response(m_engine->show_ground());
        }
        if (cmd == "set_show_ground") {
            m_engine->set_show_ground(req.at("value").get<bool>());
            // Ground grid is rasterizer-only; PT inset is unaffected, so no
            // need to flag m_clear_next_frame.
            return ok_response_null();
        }
        if (cmd == "set_gizmo_visible") {
            m_engine->set_gizmo_visible(req.at("value").get<bool>());
            return ok_response_null();
        }
        if (cmd == "set_viewport_grab_active") {
            const bool v = req.at("value").get<bool>();
            m_viewport_grab_active = v;
            // Reset the change-detection cache so the very next tick
            // re-broadcasts the current position to the JS grab.
            m_last_broadcast_mouse_x = -1.0f;
            m_last_broadcast_mouse_y = -1.0f;
            // One-line diagnostic so we can confirm the IPC reached the
            // binary that's actually running. Remove once the gate has
            // proven itself across both Debug + Release builds.
            std::fprintf(stderr, "[grab] viewport_grab_active = %d\n", v ? 1 : 0);
            return ok_response_null();
        }
        if (cmd == "set_gizmo_anchor") {
            const float x = req.value("x", 0.0f);
            const float y = req.value("y", 0.0f);
            const float z = req.value("z", 0.0f);
            const float length = req.value("length", 1.0f);
            m_engine->set_gizmo_anchor(x, y, z, length);
            return ok_response_null();
        }
        if (cmd == "get_background_color") {
            float r, g, b, a;
            m_engine->background_color(r, g, b, a);
            return ok_response(json::array({r, g, b, a}));
        }
        if (cmd == "set_background_color") {
            const auto& v = req.at("value");
            // Accept either [r,g,b] (alpha defaults to 1) or [r,g,b,a] so
            // the frontend's color-picker UI can stay 3-channel without
            // round-tripping the alpha bit.
            if (!v.is_array() || (v.size() != 3 && v.size() != 4)) {
                return err_response("background_color: expected [r,g,b] or [r,g,b,a]");
            }
            const float r = v[0].get<float>();
            const float g = v[1].get<float>();
            const float b = v[2].get<float>();
            const float a = v.size() == 4 ? v[3].get<float>() : 1.0f;
            m_engine->set_background_color(r, g, b, a);
            return ok_response_null();
        }
        if (cmd == "get_pt_preview") {
            return ok_response(m_pt_preview_enabled);
        }
        if (cmd == "set_pt_preview") {
            const bool v = req.at("value").get<bool>();
            const bool was_enabled = m_pt_preview_enabled;
            m_pt_preview_enabled = v;
            // Keep the engine's BVH-build flag aligned so the next
            // compile_scene either builds BLAS+TLAS or skips them.
            if (m_engine) m_engine->set_build_acceleration_structures(v);
            if (v && !was_enabled) {
                // OFF→ON: the live CompiledScene was built without a
                // BVH so the path tracer has nothing to trace against.
                // Re-run compile_scene against the current scene so the
                // BLAS/TLAS/material programs come up. Synchronous on
                // purpose — the user just asked for PT and expects to
                // see it on the next tick.
                if (m_engine) {
                    try { m_engine->compile_scene(); }
                    catch (const std::exception& e) {
                        std::fprintf(stderr,
                            "[set_pt_preview] compile_scene failed: %s\n", e.what());
                    }
                }
                // Accumulator may hold stale pixels from before the
                // preview was disabled (camera / scene moved since).
                // Clear on the next render so the user doesn't see
                // ghosted geometry for the first sample.
                m_clear_next_frame = true;
            }
            return ok_response_null();
        }
        if (cmd == "get_pt_fullscreen") {
            return ok_response(m_pt_fullscreen);
        }
        if (cmd == "reset_pt_accumulator") {
            // Restart accumulation from sample 0 on the next tick. Used by
            // the Render workspace's "Reset Render" button after the user
            // changes settings that don't otherwise dirty the accumulator
            // (e.g. tweaking light intensity through the inspector).
            m_clear_next_frame = true;
            return ok_response_null();
        }
        if (cmd == "set_pt_fullscreen") {
            const bool v = req.at("value").get<bool>();
            m_pt_fullscreen = v;
            apply_pt_resolution();
            return ok_response_null();
        }
        if (cmd == "get_pt_render_resolution") {
            return ok_response(json{{"width",  m_pt_render_w},
                                    {"height", m_pt_render_h}});
        }
        if (cmd == "set_pt_render_resolution") {
            // Width / height of 0 (or absent) clears the override and
            // returns to "match viewport pixel size".
            m_pt_render_w = req.value("width", 0u);
            m_pt_render_h = req.value("height", 0u);
            apply_pt_resolution();
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

        // ── Material library (project + global scopes) ──
        // Each material lives as a .json file in either the project's
        // local `materials/` folder (preferred — moves with the
        // project) or the user-wide global library (palette shared
        // across projects). Commands take an optional `scope` arg:
        //   "project"   — operate against m_project_dir/materials/
        //   "global"    — operate against global_material_dir()
        // Default scope is "project" when a project is open, "global"
        // otherwise so legacy single-file workflows keep working.
        // list_material_library returns both sets in one payload with
        // a `scope` field per entry so the frontend picker can group
        // them visually.
        auto default_save_scope = [this]() -> std::string {
            return m_project_dir.empty() ? "global" : "project";
        };
        auto scope_dir = [this](const std::string& scope) -> std::filesystem::path {
            if (scope == "project") return project_material_dir();
            return global_material_dir();
        };
        if (cmd == "list_material_library") {
            // Returns array of {name, scope}. A name appearing in both
            // scopes shows up twice — the project entry is the one
            // that wins at cook time per resolve_material_path. The UI
            // can dedupe / annotate as it sees fit.
            json arr = json::array();
            auto enumerate = [&](const std::filesystem::path& dir, const char* scope) {
                if (dir.empty()) return;
                std::error_code ec;
                if (!std::filesystem::exists(dir, ec)) return;
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (!entry.is_regular_file()) continue;
                    if (entry.path().extension() != ".json") continue;
                    arr.push_back({
                        {"name",  entry.path().stem().string()},
                        {"scope", scope},
                    });
                }
            };
            enumerate(project_material_dir(), "project");
            enumerate(global_material_dir(),  "global");
            // Stable sort by (scope, name) so the picker order is
            // consistent across reloads.
            std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
                if (a.at("scope") != b.at("scope")) return a.at("scope") < b.at("scope");
                return a.at("name") < b.at("name");
            });
            return ok_response(arr);
        }
        if (cmd == "save_material_graph_as") {
            const auto name = req.at("name").get<std::string>();
            const auto graph_json = req.at("graph").get<std::string>();
            const auto scope = req.value("scope", default_save_scope());
            if (!is_safe_library_name(name)) return err_response("invalid library name");
            if (scope != "project" && scope != "global") {
                return err_response("scope must be 'project' or 'global'");
            }
            if (scope == "project" && m_project_dir.empty()) {
                return err_response("no project open — pass scope: 'global' or open/save a project first");
            }
            // Pretty-print so the file is git-diff-friendly.
            auto graph = tracey::deserializeShaderGraph(graph_json);
            if (!graph) return err_response("could not parse graph json");
            const std::string pretty = tracey::serializeShaderGraphPretty(*graph);

            std::filesystem::path dir = scope_dir(scope);
            std::filesystem::create_directories(dir);
            std::ofstream out(dir / (name + ".json"));
            if (!out) return err_response("could not open file for writing");
            out << pretty;
            out.close();

            // Refresh every actor bound to this library name. Bound
            // actors might resolve through the OTHER scope (e.g. the
            // user just saved a project override of a name that's
            // currently resolving against the global library), so
            // we re-run resolve_material_path per actor and re-read
            // whichever scope actually wins now.
            bool any_bound = false;
            for (auto* actor : m_engine->scene().actors()) {
                if (!actor) continue;
                if (actor->materialLibraryName() != name) continue;
                std::ifstream in(resolve_material_path(name));
                if (!in) continue;
                std::stringstream ss;
                ss << in.rdbuf();
                actor->setMaterialGraphJson(ss.str());
                any_bound = true;
            }
            if (any_bound && m_engine->compiled_scene_ready()) {
                m_engine->compile_scene();
                m_clear_next_frame = true;
            }
            return ok_response_null();
        }
        if (cmd == "load_material_graph_from_library") {
            const auto name = req.at("name").get<std::string>();
            if (!is_safe_library_name(name)) return err_response("invalid library name");
            // Optional explicit scope — when not provided, falls back
            // to the resolve helper (project first, then global) so a
            // simple "open this material" still does the right thing.
            std::filesystem::path file;
            if (auto it = req.find("scope"); it != req.end() && it->is_string()) {
                const std::string scope = it->get<std::string>();
                if (scope != "project" && scope != "global") {
                    return err_response("scope must be 'project' or 'global'");
                }
                file = scope_dir(scope) / (name + ".json");
            } else {
                file = resolve_material_path(name);
            }
            std::ifstream in(file);
            if (!in) return err_response("graph not found in library");
            std::stringstream ss;
            ss << in.rdbuf();
            return ok_response(ss.str());
        }
        if (cmd == "delete_material_graph_from_library") {
            const auto name = req.at("name").get<std::string>();
            const auto scope = req.value("scope", default_save_scope());
            if (!is_safe_library_name(name)) return err_response("invalid library name");
            if (scope != "project" && scope != "global") {
                return err_response("scope must be 'project' or 'global'");
            }
            std::error_code ec;
            std::filesystem::remove(scope_dir(scope) / (name + ".json"), ec);
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
                std::ifstream in(resolve_material_path(name));
                if (!in) return err_response("library entry not found");
                std::stringstream ss;
                ss << in.rdbuf();
                graph_json = ss.str();
            }
            actor->setMaterialGraphJson(graph_json);
            actor->setMaterialLibraryName(name);

            // Mirror the assignment back onto the originating object_output
            // SOP node's `material_library_name` param. Without this, the
            // next cook re-emits this actor with an empty libraryName (the
            // SOP node is the cook-side source of truth), and apply_emitted
            // dutifully wipes the assignment we just made. Cross-tracked
            // via m_sop_node_to_actor: walk it to find the SOP uid that
            // emitted this actor. The map is small enough that the linear
            // scan is fine on the command-handler thread.
            if (m_sop_graph) {
                for (const auto& [sopUid, actorUid] : m_sop_node_to_actor) {
                    if (actorUid != id) continue;
                    if (auto* node = findNodeRecursive(m_sop_graph.get(), sopUid)) {
                        node->setParamString("material_library_name", name);
                    }
                    break;
                }
                // The change is local-only (no broadcast / no flushSopGraph)
                // — the param value sticks in m_sop_graph and the next cook
                // request reads it; we don't need to push a fresh graph
                // JSON to the worker right now because the user didn't
                // change the cook output, just the actor-side material.
            }

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
                    json pj = {
                        {"name",     p.name},
                        {"type",     tracey::sops::paramTypeName(p.type)},
                        {"default",  p.defaultRepr},
                    };
                    // UI hints — omitted entirely when unset so the wire
                    // stays lean for the (still-typical) plain-input case.
                    if (p.rangeMin != p.rangeMax) {
                        pj["range"] = {
                            {"min",  p.rangeMin},
                            {"max",  p.rangeMax},
                            {"step", p.rangeStep},
                        };
                    }
                    if (!p.options.empty()) pj["options"] = p.options;
                    params.push_back(std::move(pj));
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
            // Frontend sets cook=false for position-only edits (dragging a
            // node around the canvas) so we update the canonical graph without
            // re-running geometry. Default true preserves prior behaviour for
            // any caller that doesn't supply the flag.
            const bool cook = req.value("cook", true);
            // Parse synchronously so JSON errors surface in the response. The
            // resulting graph is stored as the canonical one (used by
            // get_sop_graph / set_actor_transform writeback). The cook itself
            // runs on a worker; the result is applied on the next render_tick.
            std::unique_ptr<tracey::sops::SopGraph> parsed;
            try {
                parsed = tracey::sops::deserializeSopGraph(graph_json);
            } catch (const std::exception& e) {
                return err_response(std::string("sop graph parse error: ") + e.what());
            }
            m_sop_graph = std::move(parsed);
            // Cache so the auto re-cook in render_tick can re-post without
            // a round-trip to the frontend.
            m_last_pushed_graph_json = graph_json;
            // Refresh the dop_import gate eagerly so the very first cook
            // after the user adds a dop_import SOP picks up its stamp.
            // The post-cook refresh in apply_emitted runs later, but
            // collect_dop_stamps in post_cook_request below reads the
            // flag NOW.
            m_has_dop_imports = detect_dop_imports();
            if (cook) {
                post_cook_request(graph_json, m_timeline.current_time);
            }
            return ok_response_null();
        }

        // ── VOP graph (per-host attribute_vop sub-graph) ──
        // Catalog mirrors list_sop_node_catalog. get/set are scoped per host
        // SOP node uid; the host must be an attribute_vop SOP.
        if (cmd == "list_vop_node_catalog") {
            // Catalog ships static metadata, but per-port DataType lives on
            // each node's runtime ports() — the catalog's PortSpec only
            // carries a name. Probe each kind with a throwaway instance to
            // pull the typed port info and emit it alongside the name so
            // the inspector knows which widget (float/int/vec3) to render
            // for each unconnected input.
            auto dataTypeName = [](tracey::DataType dt) -> std::string {
                switch (dt) {
                    case tracey::DataType::Float: return "float";
                    case tracey::DataType::Int:   return "int";
                    case tracey::DataType::Bool:  return "bool";
                    case tracey::DataType::Vec2:  return "vec2";
                    case tracey::DataType::Vec3:  return "vec3";
                    case tracey::DataType::Vec4:  return "vec4";
                    default:                      return "unknown";
                }
            };
            json arr = json::array();
            for (const auto& e : tracey::vops::VopRegistry::instance().catalog()) {
                // Probe instance — uid 0 since we never wire it into a graph.
                auto probe = tracey::vops::VopRegistry::instance().create(e.kind, 0);
                tracey::InputsAndOutputs io = probe ? probe->ports() : tracey::InputsAndOutputs{};
                const auto probeIns  = io.inputs();
                const auto probeOuts = io.outputs();
                json inputs = json::array();
                for (size_t i = 0; i < e.inputs.size(); ++i) {
                    json pj = {{"name", e.inputs[i].name}};
                    if (i < probeIns.size())
                        pj["data_type"] = dataTypeName(probeIns[i].getDataType());
                    inputs.push_back(std::move(pj));
                }
                json outputs = json::array();
                for (size_t i = 0; i < e.outputs.size(); ++i) {
                    json pj = {{"name", e.outputs[i].name}};
                    if (i < probeOuts.size())
                        pj["data_type"] = dataTypeName(probeOuts[i].getDataType());
                    outputs.push_back(std::move(pj));
                }
                json params = json::array();
                for (const auto& p : e.params) {
                    json pj = {
                        {"name",     p.name},
                        {"type",     tracey::vops::paramTypeName(p.type)},
                        {"default",  p.defaultRepr},
                    };
                    if (p.rangeMin != p.rangeMax) {
                        pj["range"] = {
                            {"min",  p.rangeMin},
                            {"max",  p.rangeMax},
                            {"step", p.rangeStep},
                        };
                    }
                    if (!p.options.empty()) pj["options"] = p.options;
                    params.push_back(std::move(pj));
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
        if (cmd == "get_vop_graph") {
            const size_t host_uid = req.at("host_uid").get<size_t>();
            // SOP first (attribute_vop), then DOP fallback (pop_force).
            // The two host kinds share the same VOP IPC since the wire
            // payload is identical — a VopGraph JSON document.
            if (m_sop_graph) {
                if (auto* sn = findNodeRecursive(m_sop_graph.get(), host_uid)) {
                    if (const auto* vop = tracey::sops::attributeVopGraph(sn))
                        return ok_response(tracey::vops::serializeVopGraph(*vop));
                }
            }
            if (m_dop_graph) {
                if (auto* dn = findDopNode(m_dop_graph.get(), host_uid)) {
                    if (const auto* vop = tracey::dops::popForceVopGraph(dn))
                        return ok_response(tracey::vops::serializeVopGraph(*vop));
                }
            }
            return err_response("host node not found or has no vop graph");
        }
        if (cmd == "set_vop_graph") {
            const size_t host_uid = req.at("host_uid").get<size_t>();
            const auto graph_json = req.at("graph").get<std::string>();
            // `cook` defaults to true. The frontend's position-only
            // pushes (node-drag-on-canvas) set it to false: the layout
            // change must persist through save_scene but doesn't
            // affect the cook output, so we skip the worker request +
            // the sop_graph_changed broadcast that would otherwise
            // trigger downstream cache invalidation.
            const bool cook = req.value("cook", true);

            // Try SOP host first.
            tracey::sops::SopNode* sop_host = nullptr;
            if (m_sop_graph)
                sop_host = findNodeRecursive(m_sop_graph.get(), host_uid);
            if (sop_host && tracey::sops::attributeVopGraph(sop_host)) {
                try {
                    auto parsed = tracey::vops::deserializeVopGraph(graph_json);
                    if (!parsed) return err_response("vop graph parse returned null");
                    tracey::sops::setAttributeVopGraph(sop_host, std::move(parsed));
                } catch (const std::exception& e) {
                    return err_response(std::string("vop graph parse error: ") + e.what());
                }
                tracey::sops::syncPromotedHostValuesFromVop(sop_host);
                if (m_sop_graph) {
                    std::string json = tracey::sops::serializeSopGraph(*m_sop_graph);
                    m_last_pushed_graph_json = json;
                    if (cook) {
                        post_cook_request(std::move(json), m_timeline.current_time);
                    }
                }
                if (cook && m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
                return ok_response_null();
            }

            // DOP host (pop_force). Mutating the DOP-hosted VOP subnet
            // invalidates the cached sim from this frame forward (it
            // changes the per-particle force expression). Wipe the frame
            // cache so the next playhead read re-sims from frame 0.
            tracey::dops::DopNode* dop_host = nullptr;
            if (m_dop_graph)
                dop_host = findDopNode(m_dop_graph.get(), host_uid);
            if (dop_host && tracey::dops::popForceVopGraph(dop_host)) {
                try {
                    auto parsed = tracey::vops::deserializeVopGraph(graph_json);
                    if (!parsed) return err_response("vop graph parse returned null");
                    tracey::dops::setPopForceVopGraph(dop_host, std::move(parsed));
                } catch (const std::exception& e) {
                    return err_response(std::string("vop graph parse error: ") + e.what());
                }
                if (cook) {
                    if (m_dop_graph) m_dop_graph->markDirty();
                    // Re-cook the SOP graph at the current frame so dop_import
                    // gets a fresh stamp from the now-invalidated sim cache.
                    if (m_sop_graph && !m_last_pushed_graph_json.empty()) {
                        post_cook_request(m_last_pushed_graph_json,
                                          m_timeline.current_time);
                    }
                    if (m_broadcast) {
                        m_broadcast(R"({"event":"dop_graph_changed"})");
                        m_broadcast(R"({"event":"dop_status","cached_to_frame":0,"current_frame":0})");
                    }
                }
                return ok_response_null();
            }
            return err_response("host node not found or has no vop graph");
        }

        if (cmd == "vop_promote_param") {
            // Promote a VOP-side parameter to a first-class param on the
            // host attribute_vop SOP node. Once promoted, the host param is
            // editable + animatable through the existing SOP keyframe path;
            // cookAt stamps the time-sampled value back into the inner VOP
            // node before evaluation. Returns the auto-generated host param
            // name so the frontend can highlight the new row.
            if (!m_sop_graph) return err_response("no sop graph");
            const size_t host_uid     = req.at("host_uid").get<size_t>();
            const size_t vop_node_uid = req.at("vop_node_uid").get<size_t>();
            const auto   param_name   = req.at("param_name").get<std::string>();

            auto* node = findNodeRecursive(m_sop_graph.get(), host_uid);
            if (!node) return err_response("host node not found");
            if (node->kind() != "attribute_vop")
                return err_response("host is not an attribute_vop");

            const std::string hostName = tracey::sops::promoteAttributeVopParam(
                node, vop_node_uid, param_name);
            if (hostName.empty()) {
                return err_response("could not promote — VOP node or param not found");
            }
            m_timeline_dirty = true;
            if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            return ok_response({{"host_param_name", hostName}});
        }
        if (cmd == "set_vop_input_default" || cmd == "clear_vop_input_default") {
            // Per-input-port constant editor: when an input has no wire,
            // the VOP graph's readInput falls back to this stored value
            // instead of returning nullopt. Lets the user dial in a
            // constant without dragging a Constant node + wire it up.
            //
            // Payload: { host_uid, vop_node_uid, port, type?, value? }.
            // `clear_vop_input_default` drops the stored value entirely so
            // the input goes back to the node's built-in zero default.
            if (!m_sop_graph) return err_response("no sop graph");
            const size_t host_uid     = req.at("host_uid").get<size_t>();
            const size_t vop_node_uid = req.at("vop_node_uid").get<size_t>();
            const size_t port         = req.at("port").get<size_t>();

            auto* node = findNodeRecursive(m_sop_graph.get(), host_uid);
            if (!node) return err_response("host node not found");
            auto* vop = tracey::sops::attributeVopGraph(node);
            if (!vop) return err_response("host is not an attribute_vop");
            auto* vopNode = vop->findNode(vop_node_uid);
            if (!vopNode) return err_response("vop node not found");

            if (cmd == "clear_vop_input_default") {
                vopNode->clearInputDefault(port);
            } else {
                const auto t = req.at("type").get<std::string>();
                const auto& v = req.at("value");
                tracey::vops::Value val;
                if (t == "float" && v.is_number())             val = v.get<float>();
                else if (t == "int" && v.is_number_integer())  val = v.get<int>();
                else if (t == "vec3" && v.is_array() && v.size() == 3)
                    val = tracey::Vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
                else return err_response("unsupported input default type");
                vopNode->setInputDefault(port, val);
            }
            vop->markDirty();
            // Re-cook so the new constant flows through to the geometry.
            if (m_sop_graph) {
                std::string json = tracey::sops::serializeSopGraph(*m_sop_graph);
                m_last_pushed_graph_json = json;
                post_cook_request(std::move(json), m_timeline.current_time);
            }
            if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            return ok_response_null();
        }
        if (cmd == "vop_demote_param") {
            // Strip a promotion + its host param. Any channels on it are
            // discarded — the user can re-promote and re-key if needed.
            if (!m_sop_graph) return err_response("no sop graph");
            const size_t host_uid        = req.at("host_uid").get<size_t>();
            const auto   host_param_name = req.at("host_param_name").get<std::string>();

            auto* node = findNodeRecursive(m_sop_graph.get(), host_uid);
            if (!node) return err_response("host node not found");
            if (node->kind() != "attribute_vop")
                return err_response("host is not an attribute_vop");

            const bool removed = tracey::sops::demoteAttributeVopParam(node, host_param_name);
            if (!removed) return ok_response(false);
            m_timeline_dirty = true;
            if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            return ok_response(true);
        }

        // ── DOP graph (top-level simulation network) ──
        // Peer of the root SOP graph; not nested inside a SOP. cookFrame
        // produces SimState[frame] from SimState[frame-1]; the frame cache
        // lives on the DopGraph itself.
        if (cmd == "list_dop_node_catalog") {
            // Probe each kind for runtime port types (matches the VOP catalog
            // handler so the inspector can pick the right input widget).
            auto dataTypeName = [](tracey::DataType dt) -> std::string {
                switch (dt) {
                    case tracey::DataType::Float: return "float";
                    case tracey::DataType::Int:   return "int";
                    case tracey::DataType::Bool:  return "bool";
                    case tracey::DataType::Vec2:  return "vec2";
                    case tracey::DataType::Vec3:  return "vec3";
                    case tracey::DataType::Vec4:  return "vec4";
                    default:                      return "unknown";
                }
            };
            json arr = json::array();
            for (const auto& e : tracey::dops::DopRegistry::instance().catalog()) {
                auto probe = tracey::dops::DopRegistry::instance().create(e.kind, 0);
                tracey::InputsAndOutputs io = probe ? probe->ports() : tracey::InputsAndOutputs{};
                const auto probeIns  = io.inputs();
                const auto probeOuts = io.outputs();
                json inputs = json::array();
                for (size_t i = 0; i < e.inputs.size(); ++i) {
                    json pj = {{"name", e.inputs[i].name}};
                    if (i < probeIns.size())
                        pj["data_type"] = dataTypeName(probeIns[i].getDataType());
                    inputs.push_back(std::move(pj));
                }
                json outputs = json::array();
                for (size_t i = 0; i < e.outputs.size(); ++i) {
                    json pj = {{"name", e.outputs[i].name}};
                    if (i < probeOuts.size())
                        pj["data_type"] = dataTypeName(probeOuts[i].getDataType());
                    outputs.push_back(std::move(pj));
                }
                json params = json::array();
                for (const auto& p : e.params) {
                    json pj = {
                        {"name",     p.name},
                        {"type",     tracey::dops::paramTypeName(p.type)},
                        {"default",  p.defaultRepr},
                    };
                    if (p.rangeMin != p.rangeMax) {
                        pj["range"] = {
                            {"min",  p.rangeMin},
                            {"max",  p.rangeMax},
                            {"step", p.rangeStep},
                        };
                    }
                    if (!p.options.empty()) pj["options"] = p.options;
                    params.push_back(std::move(pj));
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
        if (cmd == "get_dop_graph") {
            if (!m_dop_graph) return err_response("no dop graph");
            return ok_response(tracey::dops::serializeDopGraph(*m_dop_graph));
        }
        if (cmd == "set_dop_graph") {
            // Replace the canonical DopGraph from JSON. Always invalidates
            // the frame cache (markDirty inside deserialize → all prior
            // frames were derived from the old graph). No auto-cook in
            // Phase 0; cookToFrame fires from render_tick once the
            // particle nodes land and the timeline scrubs.
            const auto graph_json = req.at("graph").get<std::string>();
            try {
                auto parsed = tracey::dops::deserializeDopGraph(graph_json);
                if (!parsed) return err_response("dop graph parse returned null");
                m_dop_graph = std::move(parsed);
            } catch (const std::exception& e) {
                return err_response(std::string("dop graph parse error: ") + e.what());
            }
            if (m_broadcast) m_broadcast(R"({"event":"dop_graph_changed"})");
            return ok_response_null();
        }
        if (cmd == "dop_reset_cache") {
            // User-facing "Reset Sim" button. Drops the cache without
            // changing the graph; next playback re-sims from frame 0.
            if (m_dop_graph) m_dop_graph->clearCache();
            if (m_broadcast) {
                m_broadcast(R"({"event":"dop_cache_reset"})");
                m_broadcast(R"({"event":"dop_status","cached_to_frame":0,"current_frame":0})");
            }
            return ok_response_null();
        }
        if (cmd == "dop_get_status") {
            // Lightweight poll for the dopesheet's cache-coverage bar.
            const int cached = m_dop_graph ? m_dop_graph->cachedToFrame() : 0;
            return ok_response({{"cached_to_frame", cached}});
        }

        // ── Timeline / playback ──
        // Native owns the playhead. The frontend sends transport commands
        // (play/pause/seek) and listens for `timeline_tick` broadcasts.
        if (cmd == "timeline_get") {
            return ok_response({
                {"fps",          m_timeline.fps},
                {"frame_start",  m_timeline.frame_start},
                {"frame_end",    m_timeline.frame_end},
                {"current_time", m_timeline.current_time},
                {"playing",      m_timeline.playing},
                {"loop",         m_timeline.loop == LoopMode::Once     ? "once"
                                : m_timeline.loop == LoopMode::PingPong ? "pingpong"
                                                                        : "loop"},
            });
        }
        if (cmd == "timeline_set_range") {
            const double fps = req.value("fps", m_timeline.fps);
            const int fs = req.value("frame_start", m_timeline.frame_start);
            const int fe = req.value("frame_end",   m_timeline.frame_end);
            if (fps <= 0.0)   return err_response("fps must be > 0");
            if (fe < fs)      return err_response("frame_end must be >= frame_start");
            m_timeline.fps = fps;
            m_timeline.frame_start = fs;
            m_timeline.frame_end   = fe;
            // Clamp current_time to the new range.
            const double t0 = (fs - 1.0) / fps;
            const double t1 = (fe + 1 - 1.0) / fps;
            m_timeline.current_time = std::clamp(m_timeline.current_time, t0, t1);
            m_timeline_dirty = true;
            return ok_response_null();
        }
        if (cmd == "timeline_set_playhead") {
            // Seek + pause. Frontend can send either `time` (seconds) or
            // `frame` (1-based) — the conversion uses the current fps.
            if (req.contains("time")) {
                m_timeline.current_time = req.at("time").get<double>();
            } else if (req.contains("frame")) {
                const double f = req.at("frame").get<double>();
                m_timeline.current_time = (f - 1.0) / std::max(m_timeline.fps, 1e-6);
            } else {
                return err_response("timeline_set_playhead requires `time` or `frame`");
            }
            m_timeline.playing = false;
            m_timeline_dirty = true;
            return ok_response_null();
        }
        if (cmd == "timeline_play") {
            m_timeline.playing = true;
            // If we were sitting at the very end with Once loop, snap back to
            // the start so play actually moves.
            const double t0 = (m_timeline.frame_start - 1.0) / std::max(m_timeline.fps, 1e-6);
            const double t1 = (m_timeline.frame_end + 0.0) / std::max(m_timeline.fps, 1e-6);
            if (m_timeline.loop == LoopMode::Once && m_timeline.current_time >= t1) {
                m_timeline.current_time = t0;
                m_timeline_dirty = true;
            }
            // Frame-locked: render_tick won't advance the playhead, so it
            // also won't trigger the first cook. Kick one off here so the
            // drain → advance → post chain has somewhere to start.
            if (m_timeline.frame_locked &&
                !m_last_pushed_graph_json.empty() &&
                (m_has_animated_vop_promotions || m_has_dop_imports))
            {
                post_cook_request(m_last_pushed_graph_json, m_timeline.current_time);
            }
            return ok_response_null();
        }
        if (cmd == "timeline_pause") {
            m_timeline.playing = false;
            return ok_response_null();
        }
        if (cmd == "timeline_get_frame_locked") {
            return ok_response(m_timeline.frame_locked);
        }
        if (cmd == "timeline_set_frame_locked") {
            const bool prev = m_timeline.frame_locked;
            m_timeline.frame_locked = req.at("value").get<bool>();
            // If we just turned it ON while play is running, kick a cook
            // request so the chain keeps moving — otherwise render_tick
            // (which we just told to stop advancing) leaves the worker
            // idle until the user pauses + replays.
            if (!prev && m_timeline.frame_locked && m_timeline.playing &&
                !m_last_pushed_graph_json.empty() &&
                (m_has_animated_vop_promotions || m_has_dop_imports))
            {
                post_cook_request(m_last_pushed_graph_json, m_timeline.current_time);
            }
            return ok_response_null();
        }
        if (cmd == "timeline_set_loop") {
            const auto mode = req.value("mode", std::string{"loop"});
            if      (mode == "once")     m_timeline.loop = LoopMode::Once;
            else if (mode == "loop")     m_timeline.loop = LoopMode::Loop;
            else if (mode == "pingpong") m_timeline.loop = LoopMode::PingPong;
            else return err_response("loop mode must be one of: once, loop, pingpong");
            m_timeline.pingpong_dir = 1.0;
            return ok_response_null();
        }

        // ── Keyframe edits ──
        // Operate directly on the canonical SOP graph's parameter channels.
        // Triggers a re-eval on the next tick + a `sop_graph_changed`
        // broadcast so the frontend reloads the (now-animated) graph.
        if (cmd == "param_set_keyframe") {
            const size_t node_uid    = req.at("node_uid").get<size_t>();
            const auto   param_name  = req.at("param_name").get<std::string>();
            const int    component   = req.value("component", 0);
            const double t           = req.at("time").get<double>();
            const float  value       = req.at("value").get<float>();
            const auto   interp_str  = req.value("interp", std::string{"linear"});
            const float  in_tangent  = req.value("in_tangent",  0.0f);
            const float  out_tangent = req.value("out_tangent", 0.0f);

            if (!m_sop_graph) return err_response("no sop graph");
            auto* node = findNodeRecursive(m_sop_graph.get(),node_uid);
            if (!node) return err_response("node not found");
            auto& params = node->parameters();
            tracey::sops::Parameter* p = nullptr;
            for (auto& q : params) if (q.name == param_name) { p = &q; break; }
            if (!p) return err_response("parameter not found");

            tracey::sops::ScalarChannel::Key key;
            key.time       = t;
            key.value      = value;
            key.inTangent  = in_tangent;
            key.outTangent = out_tangent;
            key.interp     = tracey::sops::interpFromName(interp_str);
            p->channelAt(component).setKey(key);

            m_timeline_dirty = true;
            if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            return ok_response_null();
        }
        if (cmd == "param_move_keyframe") {
            // Retime a key from `from_time` to `to_time` while preserving its
            // value + tangents + interp. Used by the dopesheet's drag-key
            // interaction. If a key already sits at `to_time` it gets
            // overwritten (matches setKey's same-time-replace semantics).
            const size_t node_uid   = req.at("node_uid").get<size_t>();
            const auto   param_name = req.at("param_name").get<std::string>();
            const int    component  = req.value("component", 0);
            const double from_t     = req.at("from_time").get<double>();
            const double to_t       = req.at("to_time").get<double>();

            if (!m_sop_graph) return err_response("no sop graph");
            auto* node = findNodeRecursive(m_sop_graph.get(),node_uid);
            if (!node) return err_response("node not found");
            tracey::sops::Parameter* p = nullptr;
            for (auto& q : node->parameters())
                if (q.name == param_name) { p = &q; break; }
            if (!p) return err_response("parameter not found");
            if (component < 0 || component >= int(p->channels.size()))
                return ok_response(false);

            auto& ch = p->channels[component];
            // Snapshot the source key, remove it, then re-insert at the new
            // time. Doing it this way (instead of a direct mutate) keeps the
            // sort order valid through setKey.
            tracey::sops::ScalarChannel::Key copy{};
            bool found = false;
            for (const auto& k : ch.keys) {
                if (std::abs(k.time - from_t) <= 1e-6) { copy = k; found = true; break; }
            }
            if (!found) return ok_response(false);
            ch.removeKeyAt(from_t);
            copy.time = to_t;
            ch.setKey(copy);

            m_timeline_dirty = true;
            if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            return ok_response(true);
        }
        if (cmd == "param_delete_keyframe") {
            const size_t node_uid   = req.at("node_uid").get<size_t>();
            const auto   param_name = req.at("param_name").get<std::string>();
            const int    component  = req.value("component", 0);
            const double t          = req.at("time").get<double>();

            if (!m_sop_graph) return err_response("no sop graph");
            auto* node = findNodeRecursive(m_sop_graph.get(),node_uid);
            if (!node) return err_response("node not found");
            tracey::sops::Parameter* p = nullptr;
            for (auto& q : node->parameters())
                if (q.name == param_name) { p = &q; break; }
            if (!p) return err_response("parameter not found");
            if (component < 0 || component >= int(p->channels.size()))
                return ok_response(false);
            const bool removed = p->channels[component].removeKeyAt(t);

            m_timeline_dirty = true;
            if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            return ok_response(removed);
        }
        if (cmd == "param_clear_channel") {
            const size_t node_uid   = req.at("node_uid").get<size_t>();
            const auto   param_name = req.at("param_name").get<std::string>();
            const int    component  = req.value("component", -1);

            if (!m_sop_graph) return err_response("no sop graph");
            auto* node = findNodeRecursive(m_sop_graph.get(),node_uid);
            if (!node) return err_response("node not found");
            tracey::sops::Parameter* p = nullptr;
            for (auto& q : node->parameters())
                if (q.name == param_name) { p = &q; break; }
            if (!p) return err_response("parameter not found");

            if (component < 0) {
                p->channels.clear();
            } else if (component < int(p->channels.size())) {
                p->channels[component].keys.clear();
            }
            m_timeline_dirty = true;
            if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            return ok_response_null();
        }

        // ── Project folder ──
        // A project is a directory containing `scene.json` (the
        // serialised scene + graphs) plus a `materials/` subfolder
        // for project-local material graphs. Setting the project dir
        // doesn't itself save or load anything — it just scopes
        // subsequent material commands. save_scene / load_scene also
        // adopt the file's parent as the project dir implicitly, so
        // legacy single-file workflows still get project-scoped
        // materials for free.
        if (cmd == "get_project_dir") {
            return ok_response(m_project_dir.string());
        }
        // Make the project self-contained: walk the SOP graph for
        // external file references (currently: gltf_import paths +
        // object_output material_library_name resolving to a global
        // entry), copy them into the project folder, and rewrite the
        // SOP params to point at the local copies. After consolidate
        // succeeds the entire project folder is portable — copy it to
        // another machine, open scene.json, and everything resolves
        // against sibling files instead of paths that exist only on
        // the original host.
        //
        // Scope (v1):
        //   • gltf_import: copies the source asset folder into
        //     project_dir/assets/<source-folder-name>/. For .glb
        //     (single-file) the .glb is copied directly. For .gltf
        //     the whole containing folder gets copied so sibling
        //     .bin and texture files come along — relative URIs in
        //     the .gltf JSON keep resolving against siblings after
        //     the copy. Absolute URIs inside the .gltf JSON are NOT
        //     rewritten; consolidate flags those as warnings.
        //   • Materials: every actor with a materialLibraryName
        //     resolving from global_material_dir gets that .json
        //     copied to project_material_dir. The SOP param keeps
        //     the same name; resolve_material_path then finds the
        //     project-local copy on the next cook.
        //   • Skipped: gltf_import paths already inside
        //     m_project_dir (already local). Same for materials
        //     already in project scope.
        if (cmd == "consolidate_project") {
            if (m_project_dir.empty()) {
                return err_response("no project open — save the scene first to set a project folder");
            }
            const std::filesystem::path assetsDir = m_project_dir / "assets";
            std::error_code ec;
            std::filesystem::create_directories(assetsDir, ec);
            std::filesystem::create_directories(project_material_dir(), ec);

            json copied_assets   = json::array();
            json copied_materials = json::array();
            json warnings        = json::array();

            // Walk every SOP node (recursing into subnets) and operate
            // by kind. Lambda-recursive so we can keep the helper
            // local — the SOP graph isn't visited in this shape
            // anywhere else.
            auto same_path = [](const std::filesystem::path &a,
                                const std::filesystem::path &b) {
                std::error_code ec;
                return std::filesystem::equivalent(a, b, ec);
            };
            std::function<void(tracey::sops::SopGraph*)> walk;
            walk = [&](tracey::sops::SopGraph *g) {
                if (!g) return;
                for (const auto &nodePtr : g->nodes()) {
                    auto *node = dynamic_cast<tracey::sops::SopNode*>(nodePtr.get());
                    if (!node) continue;

                    if (node->kind() == "gltf_import") {
                        const std::string srcPath = node->paramString("path", "");
                        if (srcPath.empty()) continue;

                        std::filesystem::path src(srcPath);
                        std::error_code lec;
                        if (!std::filesystem::exists(src, lec)) {
                            warnings.push_back("missing source: " + srcPath);
                            continue;
                        }
                        // Already inside the project? Nothing to copy.
                        // weakly_canonical so the check tolerates
                        // un-normalised paths (../) and symlinks.
                        std::filesystem::path canonSrc =
                            std::filesystem::weakly_canonical(src, lec);
                        if (canonSrc.string().rfind(m_project_dir.string(), 0) == 0) {
                            continue;
                        }

                        const bool isGlb = (src.extension() == ".glb" ||
                                            src.extension() == ".GLB");
                        std::filesystem::path dstFile;
                        if (isGlb) {
                            // Single self-contained file: copy directly
                            // into assets/, no surrounding folder.
                            dstFile = assetsDir / src.filename();
                            std::filesystem::copy_file(
                                src, dstFile,
                                std::filesystem::copy_options::overwrite_existing, lec);
                            if (lec) {
                                warnings.push_back(
                                    "copy failed: " + src.string() + " → " +
                                    dstFile.string() + " (" + lec.message() + ")");
                                continue;
                            }
                        } else {
                            // .gltf + sibling .bin/textures: copy the
                            // entire containing folder so relative URIs
                            // in the JSON resolve against the copies.
                            // We namespace by source folder name so
                            // imports from different paths don't
                            // collide on filenames.
                            const std::filesystem::path srcDir = src.parent_path();
                            const std::filesystem::path dstDir =
                                assetsDir / srcDir.filename();
                            std::filesystem::create_directories(dstDir, lec);
                            std::filesystem::copy(
                                srcDir, dstDir,
                                std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing, lec);
                            if (lec) {
                                warnings.push_back(
                                    "copy failed: " + srcDir.string() + " → " +
                                    dstDir.string() + " (" + lec.message() + ")");
                                continue;
                            }
                            dstFile = dstDir / src.filename();
                        }

                        node->setParamString("path", dstFile.string());
                        copied_assets.push_back({
                            {"from", src.string()},
                            {"to",   dstFile.string()},
                        });
                    }
                    else if (node->kind() == "object_output") {
                        // Only consolidate when the SOP param names a
                        // material that resolves out of the global
                        // library. Project-scoped already, or no
                        // assignment → nothing to do.
                        const std::string matName =
                            node->paramString("material_library_name", "");
                        if (matName.empty() || !is_safe_library_name(matName)) continue;
                        std::filesystem::path resolved = resolve_material_path(matName);
                        if (resolved.empty()) {
                            warnings.push_back("material not found: " + matName);
                            continue;
                        }
                        // Skip if already project-scoped.
                        std::error_code lec;
                        std::filesystem::path localCanonResolved =
                            std::filesystem::weakly_canonical(resolved, lec);
                        std::filesystem::path projCanon =
                            std::filesystem::weakly_canonical(project_material_dir(), lec);
                        if (localCanonResolved.string().rfind(projCanon.string(), 0) == 0) {
                            continue;
                        }
                        std::filesystem::path dst =
                            project_material_dir() / (matName + ".json");
                        std::filesystem::copy_file(
                            resolved, dst,
                            std::filesystem::copy_options::overwrite_existing, lec);
                        if (lec) {
                            warnings.push_back(
                                "material copy failed: " + matName + " (" +
                                lec.message() + ")");
                            continue;
                        }
                        copied_materials.push_back(matName);
                    }

                    if (auto *inner = node->innerGraph()) walk(inner);
                }
            };
            walk(m_sop_graph.get());

            // SOP graph mutated in place — push the change back to the
            // worker's snapshot so the next cook reads the rewritten
            // paths. Without this the in-flight cook (and any cook
            // request before save_scene fires) still references the
            // original off-project files.
            m_last_pushed_graph_json = tracey::sops::serializeSopGraph(*m_sop_graph);

            // Recompile so consolidated materials take effect
            // immediately. Cook isn't required (no geometry changed),
            // just refresh the actor → material binding.
            if (!copied_materials.empty() && m_engine->compiled_scene_ready()) {
                try { m_engine->compile_scene(); }
                catch (const std::exception& e) {
                    warnings.push_back(std::string("compile_scene failed: ") + e.what());
                }
                m_clear_next_frame = true;
            }

            json result = {
                {"project_dir", m_project_dir.string()},
                {"copied_assets", std::move(copied_assets)},
                {"copied_materials", std::move(copied_materials)},
                {"warnings", std::move(warnings)},
            };
            return ok_response(result);
        }

        if (cmd == "set_project_dir") {
            const auto path = req.value("path", std::string{});
            if (path.empty()) {
                m_project_dir.clear();
                return ok_response_null();
            }
            std::filesystem::path p(path);
            std::error_code ec;
            std::filesystem::create_directories(p / "materials", ec);
            m_project_dir = std::filesystem::weakly_canonical(p, ec);
            if (ec) m_project_dir = p;  // fallback if canonicalize failed
            return ok_response(m_project_dir.string());
        }

        // ── IO ──
        // v3 schema (additive over v2):
        //   {
        //     "version": 3,
        //     "scene": <scene_state v1 payload>,
        //     "sop_graph": "<serialized SopGraph>",     // VOP graphs ride
        //                                                // along inside the
        //                                                // SOP node `extra` field
        //     "render_settings": { max_samples, max_bounces,
        //                          show_points, show_edges },
        //     "timeline": { fps, frame_start, frame_end,
        //                   current_time, loop }
        //   }
        //
        // v2 files load fine — render_settings/timeline are absent and the
        // engine keeps its defaults. v1 (pre-SOP) files load best-effort:
        // actors + camera populate the scene, no SOP graph recovered.
        if (cmd == "save_scene") {
            const auto path = req.at("path").get<std::string>();
            // Saving adopts the file's parent directory as the
            // project root. Most users will save into a freshly
            // created folder (one .tracey per folder), and that
            // folder becomes the home for any project-local
            // materials they save next. Existing flat layouts
            // (multiple .tracey files in one dir) still work — the
            // materials folder is just shared between them.
            {
                std::filesystem::path parent = std::filesystem::path(path).parent_path();
                if (!parent.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(parent / "materials", ec);
                    m_project_dir = std::filesystem::weakly_canonical(parent, ec);
                    if (ec) m_project_dir = parent;
                }
            }
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
            root["version"] = 3;
            root["scene"] = std::move(sceneJson);
            // Embed the SOP graph as a nested JSON object, not as a stringified
            // payload. The serializer returns a JSON document as a std::string;
            // assigning that string directly would force nlohmann::json to escape
            // its internal newlines and quotes, leaving the saved .tracey file
            // littered with \n and \" inside one giant single-line "sop_graph"
            // field. Parse-then-embed keeps the file human-readable and round-
            // trips cleanly on load (which now reads it back as an object).
            if (m_sop_graph) {
                try {
                    root["sop_graph"] = json::parse(
                        tracey::sops::serializeSopGraph(*m_sop_graph));
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[save] sop_graph reparse failed: %s\n", e.what());
                    root["sop_graph"] = json::object();
                }
            } else {
                root["sop_graph"] = json::object();
            }
            // DOP graph — same nested-object embedding as the SOP graph
            // so the saved .tracey stays human-readable. The frame
            // cache is intentionally NOT persisted (large, can be
            // re-simulated). Missing from older save files; load_scene
            // tolerates absence and falls back to an empty DopGraph.
            if (m_dop_graph) {
                try {
                    root["dop_graph"] = json::parse(
                        tracey::dops::serializeDopGraph(*m_dop_graph));
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[save] dop_graph reparse failed: %s\n", e.what());
                    root["dop_graph"] = json::object();
                }
            } else {
                root["dop_graph"] = json::object();
            }
            root["render_settings"] = {
                {"max_samples", m_engine->max_samples()},
                {"max_bounces", m_engine->max_bounces()},
                {"show_points", m_engine->show_points()},
                {"show_edges",  m_engine->show_edges()},
            };
            root["timeline"] = {
                {"fps",          m_timeline.fps},
                {"frame_start",  m_timeline.frame_start},
                {"frame_end",    m_timeline.frame_end},
                {"current_time", m_timeline.current_time},
                {"loop",         static_cast<int>(m_timeline.loop)},
            };

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
            // Adopt the file's parent as the project root. Same logic
            // as save_scene above — material commands now resolve
            // against the loaded project's local folder first, then
            // fall back to the user-wide global library.
            {
                std::filesystem::path parent = std::filesystem::path(path).parent_path();
                if (!parent.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(parent / "materials", ec);
                    m_project_dir = std::filesystem::weakly_canonical(parent, ec);
                    if (ec) m_project_dir = parent;
                }
            }

            const int version = root.value("version", 1);
            if (version >= 2) {
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

                // Accept either the new nested-object form (preferred) or the
                // legacy stringified form so older .tracey files still load.
                std::string sopJson;
                if (root.contains("sop_graph")) {
                    const auto& s = root["sop_graph"];
                    if (s.is_string()) sopJson = s.get<std::string>();
                    else if (s.is_object() && !s.empty()) sopJson = s.dump();
                }
                if (!sopJson.empty()) {
                    try {
                        m_sop_graph = tracey::sops::deserializeSopGraph(sopJson);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "[sop] load failed: %s\n", e.what());
                        m_sop_graph = std::make_unique<tracey::sops::SopGraph>(0);
                    }
                    // Cache the loaded JSON so an immediate scrub after load
                    // can re-cook (for VOP-promotion animation) without a
                    // round-trip.
                    m_last_pushed_graph_json = sopJson;

                    // DOP graph (optional — older save files don't carry one).
                    // Same accept-string-or-object compatibility shape as
                    // the SOP graph above. The frame cache is rebuilt on
                    // the next playhead scrub, so we don't persist it.
                    if (root.contains("dop_graph")) {
                        const auto& d = root["dop_graph"];
                        std::string dopJson;
                        if (d.is_string()) dopJson = d.get<std::string>();
                        else if (d.is_object() && !d.empty()) dopJson = d.dump();
                        if (!dopJson.empty()) {
                            try {
                                m_dop_graph = tracey::dops::deserializeDopGraph(dopJson);
                            } catch (const std::exception& e) {
                                std::fprintf(stderr, "[dop] load failed: %s\n", e.what());
                                m_dop_graph = std::make_unique<tracey::dops::DopGraph>(0);
                            }
                        } else {
                            m_dop_graph = std::make_unique<tracey::dops::DopGraph>(0);
                        }
                    } else {
                        // No dop_graph key at all — pre-DOP save file.
                        m_dop_graph = std::make_unique<tracey::dops::DopGraph>(0);
                    }
                    // Drop the actors that load_scene_from_file restored from
                    // the file's saved actor list — for any scene with a SOP
                    // graph (v2+) the cook below is the authoritative source
                    // of actors. Without this, every saved-then-loaded actor
                    // appears twice in the hierarchy (one bare from the
                    // restore + one SOP-emitted from the cook). Also reset
                    // our SOP-side tracking so apply_emitted's "did this
                    // actor change since last cook?" diff starts from a
                    // clean slate against the now-empty scene.
                    auto &liveScene = m_engine->scene();
                    std::vector<size_t> staleUids;
                    for (const auto &a : liveScene.actors())
                        if (a) staleUids.push_back(a->getUid());
                    for (size_t uid : staleUids) liveScene.removeActor(uid);
                    m_sop_node_to_actor.clear();
                    m_emitted_actor_to_actor.clear();
                    m_actor_signatures.clear();
                    m_has_applied_once = false;
                    // Prime m_has_dop_imports BEFORE the first cook so
                    // cook_and_apply's collect_dop_stamps actually runs
                    // for the load's initial cook. Without this, the
                    // loaded scene's first frame has no particles even
                    // when the saved graph has dop_import wired in.
                    m_has_dop_imports = detect_dop_imports();
                    cook_and_apply();
                }

                // v3: restore render + timeline state if present.
                if (root.contains("render_settings") && root["render_settings"].is_object()) {
                    const auto& rs = root["render_settings"];
                    if (rs.contains("max_samples")) m_engine->set_max_samples(rs["max_samples"].get<uint32_t>());
                    if (rs.contains("max_bounces")) m_engine->set_max_bounces(rs["max_bounces"].get<uint32_t>());
                    if (rs.contains("show_points")) m_engine->set_show_points(rs["show_points"].get<bool>());
                    if (rs.contains("show_edges"))  m_engine->set_show_edges (rs["show_edges"].get<bool>());
                }
                if (root.contains("timeline") && root["timeline"].is_object()) {
                    const auto& tl = root["timeline"];
                    m_timeline.fps          = tl.value("fps",          m_timeline.fps);
                    m_timeline.frame_start  = tl.value("frame_start",  m_timeline.frame_start);
                    m_timeline.frame_end    = tl.value("frame_end",    m_timeline.frame_end);
                    m_timeline.current_time = tl.value("current_time", m_timeline.current_time);
                    if (tl.contains("loop")) {
                        m_timeline.loop = static_cast<LoopMode>(
                            std::clamp(tl["loop"].get<int>(), 0, 2));
                    }
                    m_timeline_dirty = true;
                }
            } else {
                // Legacy v1 file: scene fields are at the root, no graphs
                // of any kind. Wipe DOP too so a load doesn't leave the
                // previous session's particle graph hanging around.
                load_scene_from_file(m_engine->scene(), path);
                m_sop_graph = std::make_unique<tracey::sops::SopGraph>(0);
                m_dop_graph = std::make_unique<tracey::dops::DopGraph>(0);
                m_has_dop_imports = false;
            }

            // Force a redraw + tell the frontend its stores are stale. The
            // SOP store listens for `sop_graph_changed`, the timeline UI for
            // `timeline_tick`, render-settings panel reads on first mount —
            // so a single `scene_changed` is the minimum kick.
            m_clear_next_frame = true;
            if (m_broadcast) {
                m_broadcast(R"({"event":"sop_graph_changed"})");
                m_broadcast(R"({"event":"dop_graph_changed"})");
                // The loaded DopGraph carries no frame cache (we don't
                // persist it). Tell the dopesheet to reset its cached-
                // frame indicator so it doesn't show a stale extent
                // left over from before the load.
                m_broadcast(R"({"event":"dop_status","cached_to_frame":0,"current_frame":0})");
                m_broadcast(R"({"event":"scene_changed"})");
                // Timeline tick with the loaded current_time so the playbar
                // refreshes immediately.
                m_broadcast(json{
                    {"event", "timeline_tick"},
                    {"time", m_timeline.current_time},
                    {"playing", m_timeline.playing},
                }.dump());
            }
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
        if (cmd == "export_video_start") {
            if (m_export_in_progress.load())
                return err_response("Export already running");

            VideoExportRequest exp;
            exp.path             = req.at("path").get<std::string>();
            exp.frame_start      = req.at("frame_start").get<int>();
            exp.frame_end        = req.at("frame_end").get<int>();
            exp.fps              = req.at("fps").get<double>();
            exp.samples_per_frame = req.at("samples_per_frame").get<int>();
            exp.max_bounces      = req.value("max_bounces", 0);
            exp.width            = req.value("width", 0);
            exp.height           = req.value("height", 0);
            exp.codec            = req.value("codec", std::string{"h264"});

            if (exp.path.empty()) return err_response("Missing output path");
            if (exp.frame_end < exp.frame_start)
                return err_response("frame_end must be >= frame_start");
            if (exp.samples_per_frame < 1)
                return err_response("samples_per_frame must be >= 1");
            if (exp.max_bounces < 0)
                return err_response("max_bounces must be >= 0");
            if (exp.fps <= 0.0) return err_response("fps must be > 0");
            if (exp.width < 0 || exp.height < 0)
                return err_response("width/height must be >= 0");
            // Even dimensions are required by H.264; ProRes is more forgiving
            // but we enforce the same rule for predictability.
            if (exp.width > 0 && (exp.width & 1))
                return err_response("width must be even");
            if (exp.height > 0 && (exp.height & 1))
                return err_response("height must be even");

            // Reap a previous worker that finished but wasn't joined yet.
            if (m_export_thread.joinable()) m_export_thread.join();
            m_export_cancel.store(false);
            m_export_in_progress.store(true);
            m_export_thread = std::thread(
                [this, r = std::move(exp)]() mutable { export_video_loop(std::move(r)); });
            return ok_response_null();
        }
        if (cmd == "export_video_cancel") {
            if (m_export_in_progress.load()) {
                m_export_cancel.store(true);
            }
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
        if (cmd == "peek_gltf") {
            // Structural-only walk of a glTF file: returns the node tree
            // (name + TRS + first-primitive mesh name) so the frontend can
            // synthesise a recursive subnet subtree on import without
            // re-parsing the file or pulling in tinygltf.
            const auto path = req.at("path").get<std::string>();
            std::vector<tracey::GltfLoader::HierarchyNode> roots;
            try {
                roots = tracey::GltfLoader::peekHierarchy(path);
            } catch (const std::exception& e) {
                return err_response(std::string("peek_gltf: ") + e.what());
            }

            // Recursive lambda-via-struct so we can capture the model name
            // path purely in the value of each emitted JSON node.
            std::function<json(const tracey::GltfLoader::HierarchyNode&)> toJson;
            toJson = [&](const tracey::GltfLoader::HierarchyNode& n) -> json {
                json out;
                out["name"] = n.name;
                out["translate"] = {n.translate.x, n.translate.y, n.translate.z};
                out["rotate_euler_deg"] = {n.rotateEulerDeg.x, n.rotateEulerDeg.y, n.rotateEulerDeg.z};
                out["scale"] = {n.scale.x, n.scale.y, n.scale.z};
                json meshNames = json::array();
                for (const auto& mn : n.meshObjectNames) meshNames.push_back(mn);
                out["mesh_names"] = std::move(meshNames);
                json children = json::array();
                for (const auto& c : n.children) children.push_back(toJson(c));
                out["children"] = std::move(children);
                return out;
            };

            json roots_json = json::array();
            for (const auto& r : roots) roots_json.push_back(toJson(r));
            return ok_response({{"path", path}, {"roots", roots_json}});
        }
        if (cmd == "set_viewport_visible") {
            if (!m_window) return err_response("No window");
            const bool vis = req.value("visible", true);
            m_window->set_viewport_visible(vis);
            m_viewport_active = vis;
            return ok_response_null();
        }

        // Native dialog commands are handled above the mutex, before this
        // switch — their runModal pumps the main run loop and can't safely
        // hold m_mutex while doing so.

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
