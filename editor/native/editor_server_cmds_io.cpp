// Project folder, scene + asset IO IPC commands — split out of editor_server.cpp's
// handle_command(). Runs with m_mutex held, inside handle_command's try
// block. Returns the response string when `cmd` is handled here,
// std::nullopt otherwise.

#include "editor_server_cmds_common.hpp"

namespace tracey_editor {

std::optional<std::string> EditorServer::handle_io_commands(
    const std::string& cmd, const json& req) {
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
                    wire_dop_sop_provider();
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
                wire_dop_sop_provider();
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
        if (cmd == "export_scene") {
            const auto path = req.at("path").get<std::string>();
            const auto format = req.at("format").get<std::string>();
            if (path.empty()) return err_response("Missing output path");

            tracey::SceneExporter::Format fmt;
            if (format == "gltf")      fmt = tracey::SceneExporter::Format::GltfJson;
            else if (format == "glb")  fmt = tracey::SceneExporter::Format::Glb;
            else if (format == "obj")  fmt = tracey::SceneExporter::Format::Obj;
            else return err_response("Unsupported geometry format: " + format);

            // Runs with m_mutex held (same lock that guards scene mutation),
            // exporting the live scene at its current cooked frame.
            std::string err;
            if (!tracey::SceneExporter::exportToFile(m_engine->scene(), path, fmt, &err))
                return err_response(err.empty() ? "Export failed" : err);
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

    return std::nullopt;
}

}  // namespace tracey_editor
