// Shared scaffolding for the per-domain IPC command modules
// (editor_server_cmds_*.cpp): engine includes, the response envelope,
// and the JSON converters / lookup helpers every module needs.
// Internal to editor/native — not an engine API.

#pragma once

#include "editor_server.hpp"
#include "json_helpers.hpp"
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

#include "path_tracer/api/path_tracer.hpp"
#include "graph/graphs/shader_graph/serialization.hpp"

#include "geometry/geometry_converter.hpp"
#include "sops/serialization.hpp"
#include "sops/sop_graph.hpp"
#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"
#include "sops/nodes/attribute_vop_sop.hpp"
#include "sops/nodes/instance_vop_sop.hpp"
#include "sops/nodes/dop_import_sop.hpp"
#include "vops/vop_graph.hpp"
#include "vops/vop_registry.hpp"
#include "vops/serialization.hpp"
#include "dops/dop_graph.hpp"
#include "dops/dop_node.hpp"
#include "dops/dop_registry.hpp"
#include "dops/serialization.hpp"
#include "dops/nodes/pop_force.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

namespace tracey_editor {

using json = nlohmann::json;

// Top-right path-tracer PiP geometry. ~25% of viewport width, square-pixel
// matched to the viewport's aspect so projection is the same in both views.
// Returns {x, y, w, h} in swapchain pixels.
struct InsetRect { int32_t x, y; uint32_t w, h; };
inline InsetRect compute_inset_rect(uint32_t vp_w, uint32_t vp_h) {
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

inline json actor_to_json(const tracey::Actor& a, size_t sourceSopNodeUid) {
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
        out["light"] = light_to_json(*lt);
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

inline json instance_to_json(const tracey::SceneInstance& inst) {
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

inline json mesh_info_to_json(const tracey::SceneObject& obj) {
    return {
        {"name", obj.name()},
        {"vertex_count", obj.vertexCount()},
        {"triangle_count", obj.triangleCount()},
        {"has_indices", obj.hasIndices()},
        {"has_normals", obj.hasNormals()},
        {"has_uvs", obj.hasUvs()},
    };
}

inline json texture_info_to_json(const std::string& id, const tracey::EmbeddedTexture& tex) {
    return {
        {"id", id},
        {"width", tex.width},
        {"height", tex.height},
        {"channels", tex.channels},
        {"mime_type", tex.mimeType},
    };
}

inline std::string ok_response(const json& data) {
    return json{{"ok", true}, {"data", data}}.dump();
}

inline std::string ok_response_null() {
    return json{{"ok", true}, {"data", nullptr}}.dump();
}

inline std::string err_response(const std::string& message) {
    return json{{"ok", false}, {"error", message}}.dump();
}

// Names keep the file format simple (one .json per graph) so the directory
// stays git-diff-friendly. We sanitize names to keep paths bounded.

// User-wide material library — palette shared across projects. Matches
// the legacy `material_library_dir` location so previously-saved
// materials remain reachable as global entries.
inline std::filesystem::path global_material_dir() {
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

inline bool is_safe_library_name(const std::string& name) {
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
inline tracey::sops::SopNode* findNodeRecursive(tracey::sops::SopGraph* g, size_t uid) {
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
inline tracey::dops::DopNode* findDopNode(tracey::dops::DopGraph* g, size_t uid) {
    if (!g) return nullptr;
    return g->findNode(uid);
}

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

}  // namespace tracey_editor
