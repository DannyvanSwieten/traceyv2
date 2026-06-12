// Timeline / playback + keyframe edits IPC commands — split out of editor_server.cpp's
// handle_command(). Runs with m_mutex held, inside handle_command's try
// block. Returns the response string when `cmd` is handled here,
// std::nullopt otherwise.

#include "editor_server_cmds_common.hpp"

namespace tracey_editor {

std::optional<std::string> EditorServer::handle_timeline_commands(
    const std::string& cmd, const json& req) {
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
                (m_has_animated_sop_params || m_has_dop_imports))
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
                (m_has_animated_sop_params || m_has_dop_imports))
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
        if (cmd == "param_set_channel_extrap") {
            // Set a channel's pre/post extrapolation mode (hold|linear|cycle).
            // Both fields are optional so the curve editor's menu can change
            // one side without touching the other. Extrap on a channel that
            // has no keys is meaningless, so an out-of-range component is a
            // soft failure (false) rather than an error.
            const size_t node_uid   = req.at("node_uid").get<size_t>();
            const auto   param_name = req.at("param_name").get<std::string>();
            const int    component  = req.value("component", 0);

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
            if (req.contains("pre"))
                ch.pre = tracey::sops::extrapFromName(req.at("pre").get<std::string>());
            if (req.contains("post"))
                ch.post = tracey::sops::extrapFromName(req.at("post").get<std::string>());

            m_timeline_dirty = true;
            if (m_broadcast) m_broadcast(R"({"event":"sop_graph_changed"})");
            return ok_response(true);
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

    return std::nullopt;
}

}  // namespace tracey_editor
