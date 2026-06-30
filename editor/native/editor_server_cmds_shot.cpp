// USD shot-mode IPC commands — the "parallel / opt-in" department-layer workflow
// (see docs/pipeline_layout.md, .claude/plans/sleepy-roaming-sloth.md Phase 1b).
// Owns: create_shot / open_shot / close_shot / set_active_department / save_shot /
// get_shot_state. When a shot is open, edits in the scene handlers ALSO author USD
// opinions into the active department layer (see editor_server_cmds_scene.cpp).
//
// Runs with m_mutex held, inside handle_command's try block.

#include "editor_server_cmds_common.hpp"

#include <unordered_set>

namespace tracey_editor {

#ifdef TRACEY_HAS_USD
namespace {
    // Default shot location when the frontend supplies no explicit path: under the
    // open project, else a temp dir. Parent dirs are created (the department layer
    // files land alongside shot.usda). Lets the minimal UI work without a file dialog.
    std::string default_shot_path(const std::filesystem::path& projectDir) {
        // Land under the scaffolded shots/<seq>/<shot>/ layout (docs/pipeline_layout.md)
        // when a project is open; else a temp dir. The department layer files
        // (layout/anim/lighting.usda) are written alongside shot.usda.
        std::filesystem::path base = !projectDir.empty()
            ? projectDir / "shots" / "seq01" / "sh01"
            : std::filesystem::temp_directory_path() / "tracey_shot";
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        return (base / "shot.usda").string();
    }

    json shot_state_json(const tracey::StageDocument* doc, bool shotMode) {
        json j;
        j["open"] = shotMode && doc != nullptr;
        j["departments"] = json::array();
        j["active"] = nullptr;
        j["name"] = nullptr;
        if (doc) {
            for (const auto& d : doc->departments()) j["departments"].push_back(d);
            if (!doc->activeDepartment().empty()) j["active"] = doc->activeDepartment();
            const std::string nm = doc->shotName();
            if (!nm.empty()) j["name"] = nm;
        }
        return j;
    }
}
#endif

std::optional<std::string> EditorServer::handle_shot_commands(
    const std::string& cmd, const json& req) {
    static const std::unordered_set<std::string> kShotCmds = {
        "create_shot", "open_shot", "close_shot",
        "set_active_department", "save_shot", "get_shot_state", "reference_asset",
        "shot_key_actor", "get_shot_actor_keys"};
    if (!kShotCmds.count(cmd)) return std::nullopt;

#ifndef TRACEY_HAS_USD
    return err_response("USD shot mode requires a build with OpenUSD (TRACEY_WITH_USD)");
#else
    if (cmd == "create_shot") {
        std::string path = req.value("path", std::string{});
        if (path.empty()) path = default_shot_path(m_project_dir);
        std::vector<std::string> depts;
        if (req.contains("departments") && req["departments"].is_array())
            for (const auto& d : req["departments"]) depts.push_back(d.get<std::string>());
        // Default department layers, strongest opinion first (matches the
        // DepartmentBar + docs/pipeline_layout.md). fx comes when it gets a department.
        if (depts.empty()) depts = {"render", "lighting", "anim", "layout"};
        auto doc = tracey::StageDocument::createShot(path, depts);
        if (!doc) return err_response("create_shot: failed to create " + path);
        if (!doc->save()) return err_response("create_shot: failed to save layers");
        m_stage_doc = std::move(doc);
        m_shot_mode = true;
        m_shot_path = path;
        compose_shot_into_engine();
        return ok_response(shot_state_json(m_stage_doc.get(), m_shot_mode));
    }
    if (cmd == "open_shot") {
        std::string path = req.value("path", std::string{});
        if (path.empty()) path = default_shot_path(m_project_dir);
        auto doc = tracey::StageDocument::openShot(path);
        if (!doc) return err_response("open_shot: failed to open " + path);
        m_stage_doc = std::move(doc);
        m_shot_mode = true;
        m_shot_path = path;
        compose_shot_into_engine();
        return ok_response(shot_state_json(m_stage_doc.get(), m_shot_mode));
    }
    if (cmd == "close_shot") {
        m_stage_doc.reset();
        m_shot_mode = false;
        m_shot_path.clear();
        if (m_broadcast)
            m_broadcast(json{{"event", "shot_state"},
                             {"state", shot_state_json(nullptr, false)}}.dump());
        return ok_response(shot_state_json(nullptr, false));
    }
    if (cmd == "set_active_department") {
        if (!m_stage_doc) return err_response("set_active_department: no shot open");
        const std::string dept = req.value("department", std::string{});
        if (!m_stage_doc->setActiveDepartment(dept))
            return err_response("set_active_department: unknown department '" + dept + "'");
        if (m_broadcast)
            m_broadcast(json{{"event", "shot_state"},
                             {"state", shot_state_json(m_stage_doc.get(), m_shot_mode)}}.dump());
        return ok_response(shot_state_json(m_stage_doc.get(), m_shot_mode));
    }
    if (cmd == "save_shot") {
        if (!m_stage_doc) return err_response("save_shot: no shot open");
        if (!m_stage_doc->save()) return err_response("save_shot: failed to save layers");
        return ok_response(true);
    }
    if (cmd == "get_shot_state") {
        return ok_response(shot_state_json(m_stage_doc.get(), m_shot_mode));
    }
    if (cmd == "reference_asset") {
        if (!m_stage_doc) return err_response("reference_asset: no shot open");
        const std::string assetPath = req.value("path", std::string{});
        if (assetPath.empty()) return err_response("reference_asset: 'path' required");
        // Set dressing belongs to the layout layer — author there, but DON'T change the
        // user's active department (importing an asset shouldn't yank you to Layout).
        // Save / route-to-layout / restore around the authoring only.
        const std::string prevActive = m_stage_doc->activeDepartment();
        for (const auto& d : m_stage_doc->departments())
            if (d == "layout") { m_stage_doc->setActiveDepartment("layout"); break; }
        const std::string base = std::filesystem::path(assetPath).stem().string();
        const std::string prim = m_stage_doc->referenceAssetAuto(assetPath, base);
        if (!prevActive.empty()) m_stage_doc->setActiveDepartment(prevActive);
        if (prim.empty()) return err_response("reference_asset: failed to author reference");
        compose_shot_into_engine(); // re-derive so the referenced geometry appears
        return ok_response(json{{"prim", prim},
                                {"shot", shot_state_json(m_stage_doc.get(), m_shot_mode)}});
    }
    if (cmd == "shot_key_actor") {
        if (!m_stage_doc) return err_response("shot_key_actor: no shot open");
        const uint64_t id = req.value("actor_id", static_cast<uint64_t>(0));
        auto* a = m_engine->scene().getActor(id);
        if (!a) return err_response("shot_key_actor: actor not found");
        // Keying is an Animation action → author the time sample into the anim layer,
        // but DON'T change the user's active department (Set Key from Layout shouldn't
        // yank them to Animation). Save / route-to-anim / restore around the authoring.
        const std::string prevActive = m_stage_doc->activeDepartment();
        for (const auto& d : m_stage_doc->departments())
            if (d == "anim") { m_stage_doc->setActiveDepartment("anim"); break; }
        const double frame = m_timeline.current_time * m_timeline.fps;
        const tracey::Transform& t = a->transform();
        // Key as TRS euler channels (consistent with the inspector keying) so rotation
        // interpolates in euler space rather than as a baked matrix.
        const bool keyed = m_stage_doc->setPrimTRSAtTime(
            a->name(), frame, t.position(), euler_xyz_deg_from_quat(t.rotation()), t.scale());
        if (!prevActive.empty()) m_stage_doc->setActiveDepartment(prevActive);
        if (!keyed) return err_response("shot_key_actor: failed to author keyframe");
        m_clear_next_frame = true;
        if (m_broadcast)
            m_broadcast(json{{"event", "shot_state"},
                             {"state", shot_state_json(m_stage_doc.get(), m_shot_mode)}}.dump());
        return ok_response(json{{"frame", frame}, {"prim", a->name()}});
    }
    if (cmd == "get_shot_actor_keys") {
        if (!m_stage_doc) return ok_response(json::array());
        const uint64_t id = req.value("actor_id", static_cast<uint64_t>(0));
        auto* a = m_engine->scene().getActor(id);
        if (!a) return ok_response(json::array());
        json arr = json::array();
        for (double t : m_stage_doc->primKeyTimes(a->name())) arr.push_back(t);
        return ok_response(arr);
    }
    return std::nullopt;
#endif
}

#ifdef TRACEY_HAS_USD
void EditorServer::compose_shot_into_engine() {
    if (!m_stage_doc) return;
    // Composed multi-layer stage → Scene (the round-trip render bridge), then adopt
    // it as the working scene and defer the GPU compile to render_tick (off-main).
    auto scene = m_stage_doc->toScene();
    if (!scene) return;
    m_engine->adoptScene(std::move(scene));
    m_pending_recompile = true;
    m_clear_next_frame = true;
    if (m_broadcast) {
        m_broadcast(R"({"event":"scene_changed"})");
        m_broadcast(json{{"event", "shot_state"},
                         {"state", shot_state_json(m_stage_doc.get(), m_shot_mode)}}.dump());
    }
}
#endif

}  // namespace tracey_editor
