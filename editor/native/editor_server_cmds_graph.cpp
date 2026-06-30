// SOP / VOP / DOP graph editing IPC commands — split out of editor_server.cpp's
// handle_command(). Runs with m_mutex held, inside handle_command's try
// block. Returns the response string when `cmd` is handled here,
// std::nullopt otherwise.

#include "editor_server_cmds_common.hpp"

#include "scene/usd_exporter.hpp" // publish_asset → USD (guarded by TRACEY_HAS_USD)

namespace tracey_editor {

std::optional<std::string> EditorServer::handle_graph_commands(
    const std::string& cmd, const json& req) {
        // ── Asset registry (R1: per-asset SOP graphs — "one graph models one object")
        // Each asset is a named SOP graph; the CURRENT asset's graph is the live
        // m_sop_graph, so the existing cook→scene→render previews it. The map stashes
        // the others; switch_asset swaps which is live.
        auto ensureCurrentAsset = [&]() {
            if (!m_current_asset_id.empty()) return;
            // Migrate the existing single global graph into the first asset.
            const std::string id = "asset" + std::to_string(m_next_asset_seq++);
            m_asset_graphs[id] = m_sop_graph ? tracey::sops::serializeSopGraph(*m_sop_graph) : std::string();
            m_asset_names[id] = "Asset 1";
            m_asset_order.push_back(id);
            m_current_asset_id = id;
        };
        auto stashCurrent = [&]() {
            if (!m_current_asset_id.empty() && m_sop_graph)
                m_asset_graphs[m_current_asset_id] = tracey::sops::serializeSopGraph(*m_sop_graph);
        };
        auto assetSummary = [&]() -> json {
            json arr = json::array();
            for (const auto& id : m_asset_order)
                arr.push_back({{"id", id}, {"name", m_asset_names.count(id) ? m_asset_names.at(id) : id}});
            return json{{"assets", arr},
                        {"current", m_current_asset_id.empty() ? json(nullptr) : json(m_current_asset_id)}};
        };

        if (cmd == "list_assets") {
            ensureCurrentAsset();
            stashCurrent();
            return ok_response(assetSummary());
        }
        if (cmd == "create_asset") {
            ensureCurrentAsset();
            stashCurrent();
            const std::string id = "asset" + std::to_string(m_next_asset_seq++);
            const std::string name = req.value("name",
                std::string("Asset ") + std::to_string(m_asset_order.size() + 1));
            m_asset_graphs[id] = std::string(); // empty (blank) graph
            m_asset_names[id] = name;
            m_asset_order.push_back(id);
            m_current_asset_id = id;
            load_sop_graph_json("");             // blank the viewport for the new asset
            if (m_broadcast) m_broadcast(R"({"event":"scene_changed"})");
            return ok_response(assetSummary());
        }
        if (cmd == "switch_asset") {
            const std::string id = req.value("id", std::string{});
            if (!m_asset_graphs.count(id)) return err_response("switch_asset: unknown asset " + id);
            ensureCurrentAsset();
            stashCurrent();
            m_current_asset_id = id;
            load_sop_graph_json(m_asset_graphs[id]);
            if (m_broadcast) {
                m_broadcast(R"({"event":"scene_changed"})");
                m_broadcast(R"({"event":"sop_graph_changed"})");
            }
            return ok_response(assetSummary());
        }
        if (cmd == "rename_asset") {
            const std::string id = req.value("id", std::string{});
            if (!m_asset_names.count(id)) return err_response("rename_asset: unknown asset " + id);
            m_asset_names[id] = req.value("name", m_asset_names[id]);
            return ok_response(assetSummary());
        }
        if (cmd == "delete_asset") {
            const std::string id = req.value("id", std::string{});
            if (!m_asset_graphs.count(id)) return err_response("delete_asset: unknown asset " + id);
            m_asset_graphs.erase(id);
            m_asset_names.erase(id);
            m_asset_order.erase(std::remove(m_asset_order.begin(), m_asset_order.end(), id), m_asset_order.end());
            if (m_current_asset_id == id) {
                m_current_asset_id = m_asset_order.empty() ? std::string() : m_asset_order.front();
                load_sop_graph_json(m_current_asset_id.empty() ? std::string()
                                                               : m_asset_graphs[m_current_asset_id]);
                if (m_broadcast) {
                    m_broadcast(R"({"event":"scene_changed"})");
                    m_broadcast(R"({"event":"sop_graph_changed"})");
                }
            }
            return ok_response(assetSummary());
        }
        // Publish the CURRENT asset's cooked geometry → <project>/assets/<name>/<name>.usd
        // (the asset's USD model). It then appears in the project Assets tab and can be
        // referenced into a shot's layout (→ Shot). This is how a modeled asset enters
        // the USD scene — composing multiple objects happens by referencing, not by one
        // graph emitting many actors.
        if (cmd == "publish_asset") {
#ifndef TRACEY_HAS_USD
            return err_response("publish_asset: this build has no OpenUSD support");
#else
            ensureCurrentAsset();
            if (m_current_asset_id.empty()) return err_response("publish_asset: no current asset");
            if (m_project_dir.empty()) return err_response("publish_asset: save the project first");
            const std::string name = m_asset_names.count(m_current_asset_id)
                                         ? m_asset_names[m_current_asset_id] : m_current_asset_id;
            // No spaces in published filenames/asset paths — they make USD asset
            // resolution fragile. "Asset 1" → "Asset_1" (matches the prim-name
            // sanitization in referenceAssetAuto, so file and reference agree).
            std::string safe;
            for (char c : name) {
                const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '_' || c == '-';
                safe += ok ? c : '_';
            }
            if (safe.empty()) safe = "asset";
            const std::filesystem::path dir = m_project_dir / "assets" / safe;
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::string path = (dir / (safe + ".usd")).string();
            std::string err;
            if (!tracey::UsdExporter::exportToFile(m_engine->scene(), path, &err))
                return err_response("publish_asset: " + err);
            if (m_broadcast) m_broadcast(R"({"event":"assets_changed"})");
            return ok_response(json{{"name", name}, {"path", path}});
#endif
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
            // Keep the current asset's stored recipe in sync (R1). Editing the live
            // graph edits the current asset; switch_asset later restores from here.
            if (!m_current_asset_id.empty()) m_asset_graphs[m_current_asset_id] = graph_json;
            // Refresh the dop_import gate eagerly so the very first cook
            // after the user adds a dop_import SOP picks up its stamp.
            // The post-cook refresh in apply_emitted runs later, but
            // collect_dop_stamps in post_cook_request below reads the
            // flag NOW.
            m_has_dop_imports = detect_dop_imports();
            // Same eager refresh for the animated-SOP-param gate that drives
            // the per-frame re-cook during playback. apply_emitted also
            // recomputes it, but its "nothing changed" early-out skips that
            // update when a cook's actors are byte-identical to the previous
            // frame — exactly what happens when auto-key writes a keyframe
            // whose value matches the current frame (0° at frame 1, or
            // 360°≡0° at the loop end). Without this the graph would be
            // animated but playback would never re-cook it.
            m_has_animated_sop_params = detect_animated_sop_params();
            // Any SOP edit potentially changes the geometry that a
            // pop_source(emit_mode=geometry) reads from. Blanket-
            // invalidate the DOP sim cache so the next playhead read
            // re-cooks from frame 0 with the fresh source geometry.
            // Cheap when there's no geometry-source DOP (markDirty is
            // ~free; the re-cook only fires the next time the timeline
            // actually advances). A surgical check (only invalidate
            // when the changed SOP is referenced by a source DOP) is a
            // v2 follow-up.
            if (cook && m_dop_graph) m_dop_graph->markDirty();
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
                    if (const auto* vop = tracey::sops::instanceVopGraph(sn))
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
            // SOP host: instance_vop (sibling of attribute_vop — same
            // VopGraph storage, different cook path). Same recompile +
            // re-cook contract because the instance buffer the rasterizer
            // and TLAS consume changes whenever the graph changes.
            if (sop_host && tracey::sops::instanceVopGraph(sop_host)) {
                try {
                    auto parsed = tracey::vops::deserializeVopGraph(graph_json);
                    if (!parsed) return err_response("vop graph parse returned null");
                    tracey::sops::setInstanceVopGraph(sop_host, std::move(parsed));
                } catch (const std::exception& e) {
                    return err_response(std::string("vop graph parse error: ") + e.what());
                }
                tracey::sops::syncPromotedInstanceVopValues(sop_host);
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

            // Both attribute_vop and instance_vop hosts can promote VOP-side
            // params (same UI, same animation contract). Pick the matching
            // helper by kind; the helpers return an empty string when the
            // VOP node / param doesn't exist on this host.
            std::string hostName;
            if (node->kind() == "attribute_vop")
                hostName = tracey::sops::promoteAttributeVopParam(node, vop_node_uid, param_name);
            else if (node->kind() == "instance_vop")
                hostName = tracey::sops::promoteInstanceVopParam(node, vop_node_uid, param_name);
            else
                return err_response("host is not an attribute_vop or instance_vop");
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
            if (!vop) vop = tracey::sops::instanceVopGraph(node);
            if (!vop) return err_response("host is not an attribute_vop or instance_vop");
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
        if (cmd == "set_joint_pose") {
            // FK pose: set one joint's local-rotation override (euler degrees)
            // for a skinned actor. Stored editor-side (drives the bone overlay)
            // and written to the owning gltf_import node's `pose_overrides`
            // param, then re-cooked so the mesh deforms. The param is the
            // persistent source of truth (saves with the .tracey). Payload:
            // { actor_id, joint, rotation: [x,y,z] }.
            if (!m_sop_graph) return err_response("no sop graph");
            const uint64_t actorId = req.at("actor_id").get<uint64_t>();
            const int joint = req.at("joint").get<int>();
            const auto& rot = req.at("rotation");
            const tracey::Vec3 euler(rot[0].get<float>(), rot[1].get<float>(),
                                     rot[2].get<float>());

            auto sit = m_actor_skeletons.find(actorId);
            if (sit == m_actor_skeletons.end() || sit->second.gltfImportNode == 0)
                return err_response("actor has no skinned import");

            auto& poses = m_joint_poses[actorId];
            if (euler == tracey::Vec3(0.0f))
                poses.erase(joint);          // identity = clear the override
            else
                poses[joint] = euler;

            // Serialize this actor's poses into the param string the SOP parses
            // ("jointIndex ex ey ez ..."), then re-cook through the owning node.
            std::string s;
            for (const auto& [j, e] : poses)
                s += std::to_string(j) + " " + std::to_string(e.x) + " " +
                     std::to_string(e.y) + " " + std::to_string(e.z) + " ";

            if (auto* node = findNodeRecursive(m_sop_graph.get(), sit->second.gltfImportNode)) {
                node->setParamString("pose_overrides", s);
                std::string json = tracey::sops::serializeSopGraph(*m_sop_graph);
                m_last_pushed_graph_json = json;
                post_cook_request(std::move(json), m_timeline.current_time);
                if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            }
            if (poses.empty()) m_joint_poses.erase(actorId);
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
            bool removed = false;
            if (node->kind() == "attribute_vop")
                removed = tracey::sops::demoteAttributeVopParam(node, host_param_name);
            else if (node->kind() == "instance_vop")
                removed = tracey::sops::demoteInstanceVopParam(node, host_param_name);
            else
                return err_response("host is not an attribute_vop or instance_vop");
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
                    if (!p.picker.empty()) pj["picker"] = p.picker;
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

    return std::nullopt;
}

}  // namespace tracey_editor
