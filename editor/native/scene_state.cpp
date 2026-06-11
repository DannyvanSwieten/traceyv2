#include "scene_state.hpp"
#include "json_helpers.hpp"

#include "scene/scene.hpp"
#include "scene/actor.hpp"

#include <json.hpp>

#include <fstream>
#include <stdexcept>

using nlohmann::json;

namespace tracey_editor {

void save_scene_to_file(const tracey::Scene& scene, const std::string& path) {
    json root;
    json actors_arr = json::array();
    for (const auto* actor : scene.actors()) {
        json a = {
            {"id", actor->getUid()},
            {"name", actor->name()},
            {"transform", transform_to_json(actor->transform())},
            {"children", json::array()},
        };
        for (size_t child_uid : actor->children()) {
            a["children"].push_back(child_uid);
        }
        // Persist the light component so manually-authored lights survive
        // save/load. SOP-emitted actors with lights will also serialize,
        // but a subsequent cook overwrites them — only matters for actors
        // created via the create_light IPC.
        if (actor->hasLight()) {
            a["light"] = light_to_json(*actor->light());
        }
        actors_arr.push_back(std::move(a));
    }
    root["actors"] = std::move(actors_arr);

    if (scene.hasCamera())
        root["camera"] = camera_to_json(scene.camera());

    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Failed to open file for writing: " + path);
    out << root.dump(2);
}

void load_scene_from_file(tracey::Scene& scene, const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("Failed to open file for reading: " + path);

    json root;
    in >> root;

    scene.clear();

    if (root.contains("camera"))
        scene.setCamera(camera_from_json(root["camera"]));

    if (root.contains("actors")) {
        for (const auto& a : root["actors"]) {
            auto* actor = scene.createActor();
            actor->setName(a.value("name", std::string{}));
            if (a.contains("transform"))
                actor->setTransform(transform_from_json(a["transform"]));
            if (a.contains("light") && !a["light"].is_null())
                actor->setLight(light_from_json(a["light"]));
        }
    }
}

}  // namespace tracey_editor
