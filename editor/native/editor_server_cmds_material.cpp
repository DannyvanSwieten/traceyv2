// Material graphs, material library + per-actor assignment IPC commands — split out of editor_server.cpp's
// handle_command(). Runs with m_mutex held, inside handle_command's try
// block. Returns the response string when `cmd` is handled here,
// std::nullopt otherwise.

#include "editor_server_cmds_common.hpp"

namespace tracey_editor {

std::optional<std::string> EditorServer::handle_material_commands(
    const std::string& cmd, const json& req) {
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

    return std::nullopt;
}

}  // namespace tracey_editor
