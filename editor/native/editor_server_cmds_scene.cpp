// Scene management + scene resource queries IPC commands — split out of editor_server.cpp's
// handle_command(). Runs with m_mutex held, inside handle_command's try
// block. Returns the response string when `cmd` is handled here,
// std::nullopt otherwise.

#include "editor_server_cmds_common.hpp"

namespace tracey_editor {

std::optional<std::string> EditorServer::handle_scene_commands(
    const std::string& cmd, const json& req) {
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

    return std::nullopt;
}

}  // namespace tracey_editor
