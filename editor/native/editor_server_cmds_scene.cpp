// Scene management + scene resource queries IPC commands — split out of editor_server.cpp's
// handle_command(). Runs with m_mutex held, inside handle_command's try
// block. Returns the response string when `cmd` is handled here,
// std::nullopt otherwise.

#include "editor_server_cmds_common.hpp"

#include "scene/usd_loader.hpp" // USD-free header; import_usd_stage guarded by TRACEY_HAS_USD

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
            actor->setTransform(xform);
            actor->setLight(light);

#ifdef TRACEY_HAS_USD
            if (m_shot_mode && m_stage_doc) {
                // Author the light into the active department layer at a stable prim
                // path, and name the actor by that path so later transform edits route
                // back to the same prim. (v1 routes every type → SphereLight; full
                // UsdLux type coverage is a lighting-department task.)
                std::string prim = "/shot/";
                for (char c : name) {
                    const bool an = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    (c >= '0' && c <= '9');
                    prim += an ? c : '_';
                }
                prim += "_" + std::to_string(actor->getUid());
                actor->setName(prim);
                // Lights belong to the lighting department — route the authoring there
                // and restore (adding a light from Animation must not land it in
                // anim.usda, nor retarget where your other edits go). defineLight
                // authors the matching UsdLux type + full params + transform, so every
                // type survives a recompose (Sun/Area used to degrade via SphereLight).
                const std::string prevActive = m_stage_doc->activeDepartment();
                for (const auto& d : m_stage_doc->departments())
                    if (d == "lighting") { m_stage_doc->setActiveDepartment("lighting"); break; }
                m_stage_doc->defineLight(prim, light, xform.toMatrix());
                if (!prevActive.empty()) m_stage_doc->setActiveDepartment(prevActive);
            } else
#endif
            {
                actor->setName(name);
            }

            // Refresh ONLY the light buffer in place — adding a light changes no
            // geometry, so a full compile_scene() (which re-uploads every vertex
            // buffer + rebuilds the TLAS) would needlessly freeze the UI on large
            // scenes like Kitchen Set.
            if (m_engine->path_tracer_ready()) m_engine->update_lights();
            m_clear_next_frame = true;
            if (m_broadcast) m_broadcast(R"({"event":"scene_changed"})");
            return ok_response(actor->getUid());
        }
        if (cmd == "import_usd_stage") {
            // Async: the stage's lights / camera / instanced geometry are
            // imported on a worker thread (import_usd_stage_worker) so a big
            // asset doesn't block the main run loop (which beachballed the
            // WebView). Returns immediately; the worker broadcasts
            // usd_import_progress / usd_import_done / usd_import_error.
            // (Regular meshes still flow through the procedural SOP path —
            // peek_usd + usd_import subnets — on the frontend.)
#ifdef TRACEY_HAS_USD
            if (m_import_in_progress.load())
                return err_response("A USD import is already running");
            UsdStageImportRequest ir;
            ir.path      = req.at("path").get<std::string>();
            ir.lights    = req.value("lights", true);
            ir.camera    = req.value("camera", true);
            ir.instances = req.value("instances", true);
            if (ir.path.empty()) return err_response("Missing path");

            if (m_import_thread.joinable()) m_import_thread.join();
            m_import_in_progress.store(true);
            m_import_thread = std::thread(
                [this, r = std::move(ir)]() mutable { import_usd_stage_worker(std::move(r)); });
            return ok_response_null();
#else
            return err_response("import_usd_stage: this build has no OpenUSD support");
#endif
        }
        if (cmd == "delete_actor") {
            // Remove a manually-created actor (e.g. a create_light dome/sun)
            // directly from the scene. SOP-emitted actors are deleted via SOP
            // node removal instead; this path is for actors with no source
            // node, which the cook never manages — so the removal is permanent
            // (apply_emitted only reconciles actors in m_emitted_actor_to_actor).
            const uint64_t id = req.at("actor_id").get<uint64_t>();
            auto* a = m_engine->scene().getActor(id);
            if (!a) return ok_response(false);
#ifdef TRACEY_HAS_USD
            if (m_shot_mode && m_stage_doc) {
                // Layout/shot: the actor is a USD-referenced instance. Removing it from
                // the engine scene alone wouldn't stick — the next recompose re-derives
                // it from the stage. Remove the instance's prim from the layout layer,
                // then recompose so it's gone for good (and persisted in layout.usd).
                const std::string primPath = a->name();
                if (!m_stage_doc->removePrim(primPath))
                    return err_response("delete_actor: could not remove " + primPath);
                if (m_selected_actor_id && *m_selected_actor_id == id) m_selected_actor_id.reset();
                compose_shot_into_engine();
                return ok_response(true);
            }
#endif
            // A light-only actor (no geometry instances — e.g. a create_light
            // sun/dome) can refresh lights in place; anything carrying geometry
            // needs the full recompile so its instances/TLAS are rebuilt.
            const bool lightOnly = a->hasLight() && a->instances().empty();
            m_engine->scene().removeActor(static_cast<size_t>(id));
            if (m_selected_actor_id && *m_selected_actor_id == id)
                m_selected_actor_id.reset();
            if (m_engine->path_tracer_ready()) {
                if (lightOnly) m_engine->update_lights();
                else m_engine->compile_scene();
            }
            m_clear_next_frame = true;
            if (m_broadcast) m_broadcast(R"({"event":"scene_changed"})");
            return ok_response(true);
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

#ifdef TRACEY_HAS_USD
            // Shot mode: sync the FULL light state to its UsdLux prim, or the edit
            // only lives on the engine actor and the next recompose / save-reopen
            // reverts it to the creation defaults. Routed to the lighting layer
            // (same as create_light); defineLight re-authors type changes too.
            // Guarded on the name being a prim path — stage-derived and shot-created
            // lights are; procedural-era lights ("Dome") are not and stay engine-only.
            if (m_shot_mode && m_stage_doc && !a->name().empty() && a->name()[0] == '/') {
                const std::string prevActive = m_stage_doc->activeDepartment();
                for (const auto& d : m_stage_doc->departments())
                    if (d == "lighting") { m_stage_doc->setActiveDepartment("lighting"); break; }
                m_stage_doc->defineLight(a->name(), light, a->transform().toMatrix());
                if (!prevActive.empty()) m_stage_doc->setActiveDepartment(prevActive);
            }
#endif

            // Light-only edit → in-place light refresh (no geometry recompile).
            if (m_engine->path_tracer_ready()) m_engine->update_lights();
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

#ifdef TRACEY_HAS_USD
            // Shot mode: also author the edit into the active department layer (for
            // persistence + non-destructive separation). The actor's name is its USD
            // prim path (set by convertStageToScene); author the full transform as a
            // matrix op (lossless — no quaternion→euler). Then refresh the TLAS in
            // place (topology unchanged) so the move is visible without a full compile.
            if (m_shot_mode && m_stage_doc) {
                if (m_stage_doc->activeDepartment() == "anim") {
                    // Auto-key: dragging in the Animation department writes a keyframe
                    // (time sample) at the playhead, not a static placement. Keyed as
                    // TRS euler channels so rotation interpolates in euler space.
                    const double frame = m_timeline.current_time * m_timeline.fps;
                    m_stage_doc->setPrimTRSAtTime(a->name(), frame, xform.position(),
                                                  euler_xyz_deg_from_quat(xform.rotation()),
                                                  xform.scale());
                    if (m_broadcast) m_broadcast(R"({"event":"shot_keys_changed"})");
                } else {
                    m_stage_doc->setPrimMatrix(a->name(), xform.toMatrix());
                }
                if (m_engine->path_tracer_ready()) m_engine->refresh_tlas_only();
            }
#endif

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

#ifdef TRACEY_HAS_USD
            // Shot mode: author the full (now-rotated) transform into the active
            // department layer, same as set_actor_transform. Without this the rotation
            // only lived on the engine actor and the next recompose reverted it.
            if (m_shot_mode && m_stage_doc) {
                if (m_stage_doc->activeDepartment() == "anim") {
                    // Key as TRS euler channels using the euler the user actually typed
                    // (deg), NOT a quat→euler round-trip — so a -180°→180° key sweeps a
                    // full 360° instead of collapsing (the two are the same orientation).
                    const double frame = m_timeline.current_time * m_timeline.fps;
                    m_stage_doc->setPrimTRSAtTime(a->name(), frame, xf.position(), deg, xf.scale());
                    if (m_broadcast) m_broadcast(R"({"event":"shot_keys_changed"})");
                } else {
                    m_stage_doc->setPrimMatrix(a->name(), xf.toMatrix());
                }
                if (m_engine->path_tracer_ready()) m_engine->refresh_tlas_only();
            }
#endif

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
            // Camera param changes (pose / fov / DOF) invalidate the
            // path-tracer accumulation — restart it next frame.
            m_clear_next_frame = true;
            return ok_response_null();
        }
        if (cmd == "get_camera") {
            if (!m_engine->scene().hasCamera()) {
                return ok_response(camera_to_json(tracey::Camera{}));
            }
            return ok_response(camera_to_json(m_engine->scene().camera()));
        }
        if (cmd == "select_actor") {
            // Record the selection ONLY — never move the camera. Selecting used
            // to snap the orbit pivot to the actor's transform origin, but that's
            // (0,0,0) for imported / instanced geometry (placement lives in the
            // INSTANCE transforms, not the actor transform), so every selection
            // swung the camera to the world origin. Framing is now an explicit
            // action (F / Shift+F / the Frame buttons). Not touching the camera
            // also means selecting doesn't reset the path-tracer accumulation.
            // Prefer the stable SOP-node id when the frontend supplies it. A
            // deforming actor (skinned mesh, animated geometry) is torn down
            // and recreated with a NEW uid every frame during playback, so the
            // frontend's cached actor_id can be a dead uid. sop_node_uid is
            // stable across cooks, so we map it to the CURRENT live actor —
            // keeping selection (and the skeleton overlay keyed off it) valid.
            std::optional<uint64_t> resolved;
            auto sopIt = req.find("sop_node_uid");
            if (sopIt != req.end() && !sopIt->is_null()) {
                const size_t sopUid = sopIt->get<size_t>();
                auto it = m_sop_node_to_actor.find(sopUid);
                if (it != m_sop_node_to_actor.end()) resolved = it->second;
            }
            std::optional<uint64_t> newSel;
            if (resolved) {
                newSel = resolved;
            } else {
                const auto& idField = req.at("actor_id");
                if (!idField.is_null()) newSel = idField.get<uint64_t>();
            }
            // Changing which actor is selected drops any joint selection (the
            // joint index belongs to the previous actor's skeleton).
            if (newSel != m_selected_actor_id) m_selected_joint = -1;
            m_selected_actor_id = newSel;
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
            // distance so the preset actually frames something — and snap the
            // smoothed pose to it so the very first preset doesn't glide in from
            // a stale default (later presets glide, handled by the tick).
            if (!m_orbit_initialized) {
                m_orbit_pivot_x = m_orbit_pivot_y = m_orbit_pivot_z = 0.0f;
                if (m_orbit_distance <= 0.0f) m_orbit_distance = 8.0f;
                m_orbit_initialized = true;
                m_orbit_smooth_yaw = m_orbit_yaw;
                m_orbit_smooth_pitch = m_orbit_pitch;
                m_orbit_smooth_distance = m_orbit_distance;
                m_orbit_smooth_pivot_x = m_orbit_pivot_x;
                m_orbit_smooth_pivot_y = m_orbit_pivot_y;
                m_orbit_smooth_pivot_z = m_orbit_pivot_z;
            }
            // Don't recompose the camera here: moving only the TARGET lets
            // update_camera_from_input() ease the camera there over a few frames
            // (a smooth glide instead of a teleport). The tick clears + re-renders
            // while it animates.
            m_clear_next_frame = true;
            return ok_response_null();
        }
        if (cmd == "frame_view") {
            // Frame the selection (selected=true → falls back to the whole scene
            // if nothing is selected) or the whole scene. Raises the same one-shot
            // the F / Shift+F keys use, so the camera GLIDES to fit via the render
            // tick — identical behaviour to the keyboard shortcut.
            const bool selected = req.value("selected", true);
            if (m_window) {
                if (selected) m_window->input().frame_selected = true;
                else m_window->input().frame_all = true;
            }
            m_clear_next_frame = true;
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
        if (cmd == "get_texture_data") {
            // Base64 of the texture's ENCODED bytes (PNG/JPEG as stored) + its
            // MIME type, so the WebView can render it directly as a data: URL —
            // no native decode/resize. The Resources browser fetches this lazily
            // (only for thumbnails scrolled into view) so a 1000-texture scene
            // doesn't transfer every image up front.
            const auto id = req.at("id").get<std::string>();
            const auto* tex = m_engine->scene().getEmbeddedTexture(id);
            if (!tex) return err_response("Texture not found: " + id);
            if (tex->data.empty()) return err_response("Texture has no data: " + id);
            return ok_response(json{
                {"mime_type", tex->mimeType.empty() ? std::string("image/png") : tex->mimeType},
                {"base64", base64_encode(tex->data.data(), tex->data.size())},
            });
        }

    return std::nullopt;
}

}  // namespace tracey_editor
