// Rendering, render settings + viewport surface IPC commands — split out of editor_server.cpp's
// handle_command(). Runs with m_mutex held, inside handle_command's try
// block. Returns the response string when `cmd` is handled here,
// std::nullopt otherwise.

#include "editor_server_cmds_common.hpp"

#include "io/denoiser.hpp"      // denoiserAvailable() for the get_denoiser_available query
#include "scene/usd_loader.hpp" // USD-free header; peek_usd handler guarded by TRACEY_HAS_USD

namespace tracey_editor {

std::optional<std::string> EditorServer::handle_render_commands(
    const std::string& cmd, const json& req) {
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
            // Record the authoritative device-pixel viewport size, then let
            // apply_pt_resolution pick the right PT size for the current state
            // (fullscreen render → match the viewport; PiP → inset rect; honour
            // an explicit render-resolution override). Previously this path
            // hardcoded the inset rect, so a resize in the Render workspace
            // reset the fullscreen PT to inset size + aspect → stretched.
            m_viewport_pixel_w = w;
            m_viewport_pixel_h = h;
            apply_pt_resolution();
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
            // Bounce depth changes the radiance of every sample, so the already-
            // accumulated samples (at the old depth) are stale — restart the
            // Welford accumulator so the new depth takes effect from sample 0.
            // (Unlike set_max_samples, which only moves the cap and keeps going.)
            m_clear_next_frame = true;
            return ok_response_null();
        }
        if (cmd == "get_pt_backend") {
            return ok_response(m_engine->pt_backend());
        }
        if (cmd == "get_denoiser_available") {
            // True only when the build linked Intel OIDN (TRACEY_WITH_OIDN). The
            // UI greys out the Denoise toggle when this is false.
            return ok_response(tracey::denoiserAvailable());
        }
        if (cmd == "set_denoise_preview") {
            // Denoise-at-convergence toggle for the viewport. Re-arm the at-cap
            // denoise so the change takes effect on the image that's already on
            // screen: turning it ON denoises the converged frame on the next
            // tick (no re-render); turning it OFF stops re-applying it.
            m_engine->set_denoise_preview(req.at("value").get<bool>());
            m_pt_denoised_at_cap = false;
            return ok_response_null();
        }
        if (cmd == "set_pt_backend") {
            m_engine->set_pt_backend(req.at("backend").get<std::string>());
            m_clear_next_frame = true;  // restart accumulation on the new backend
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
        if (cmd == "peek_usd") {
            // Structural walk of a USD stage → the same hierarchy shape as
            // peek_gltf (flat first slice: one node per mesh prim, world TRS +
            // the prim's Sdf path as the SceneObject key). The frontend's
            // buildSubnetsFromUsd turns it into usd_import → object_output
            // chains. Mirrors peek_gltf exactly so the importers share a path.
#ifdef TRACEY_HAS_USD
            const auto path = req.at("path").get<std::string>();
            std::vector<tracey::UsdLoader::HierarchyNode> roots;
            tracey::UsdLoader::StageTimeInfo timeInfo;
            try {
                roots = tracey::UsdLoader::peekHierarchy(path, &timeInfo);
            } catch (const std::exception& e) {
                return err_response(std::string("peek_usd: ") + e.what());
            }

            std::function<json(const tracey::UsdLoader::HierarchyNode&)> toJson;
            toJson = [&](const tracey::UsdLoader::HierarchyNode& n) -> json {
                json out;
                out["name"] = n.name;
                out["translate"] = {n.translate.x, n.translate.y, n.translate.z};
                out["rotate_euler_deg"] = {n.rotateEulerDeg.x, n.rotateEulerDeg.y, n.rotateEulerDeg.z};
                out["scale"] = {n.scale.x, n.scale.y, n.scale.z};
                json meshNames = json::array();
                for (const auto& mn : n.meshObjectNames) meshNames.push_back(mn);
                out["mesh_names"] = std::move(meshNames);
                // Animated world transform → per-sample TRS (timeCode units).
                // Absent/empty for static prims. The frontend bakes these into
                // keyframe channels on the subnet's transform params.
                if (!n.trsSamples.empty()) {
                    json samples = json::array();
                    for (const auto& s : n.trsSamples) {
                        samples.push_back({
                            {"t", s.timeCode},
                            {"translate", {s.translate.x, s.translate.y, s.translate.z}},
                            {"rotate_euler_deg", {s.rotateEulerDeg.x, s.rotateEulerDeg.y, s.rotateEulerDeg.z}},
                            {"scale", {s.scale.x, s.scale.y, s.scale.z}},
                        });
                    }
                    out["trs_samples"] = std::move(samples);
                }
                json children = json::array();
                for (const auto& c : n.children) children.push_back(toJson(c));
                out["children"] = std::move(children);
                return out;
            };

            json roots_json = json::array();
            for (const auto& r : roots) roots_json.push_back(toJson(r));
            // Stage time metadata → the editor timeline (fps + frame range).
            return ok_response({
                {"path", path},
                {"roots", roots_json},
                {"animated", timeInfo.hasAnimation},
                {"time_codes_per_second", timeInfo.timeCodesPerSecond},
                {"start_time_code", timeInfo.startTimeCode},
                {"end_time_code", timeInfo.endTimeCode},
            });
#else
            return err_response("peek_usd: this build has no OpenUSD support");
#endif
        }
        if (cmd == "set_viewport_visible") {
            if (!m_window) return err_response("No window");
            const bool vis = req.value("visible", true);
            m_window->set_viewport_visible(vis);
            m_viewport_active = vis;
            return ok_response_null();
        }

    return std::nullopt;
}

}  // namespace tracey_editor
