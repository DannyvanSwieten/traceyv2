#include "scene_state.hpp"

#include "scene/scene.hpp"
#include "scene/actor.hpp"
#include "scene/transform.hpp"
#include "scene/camera.hpp"
#include "scene/light.hpp"

#include <json.hpp>

#include <fstream>
#include <stdexcept>

using nlohmann::json;

namespace tracey_editor {

namespace {

json transform_to_json(const tracey::Transform& t) {
    auto p = t.position();
    auto r = t.rotation();
    auto s = t.scale();
    return {
        {"position", {{"x", p.x}, {"y", p.y}, {"z", p.z}}},
        {"rotation", {{"w", r.w}, {"x", r.x}, {"y", r.y}, {"z", r.z}}},
        {"scale", {{"x", s.x}, {"y", s.y}, {"z", s.z}}},
    };
}

tracey::Transform transform_from_json(const json& j) {
    tracey::Transform t;
    const auto& p = j.at("position");
    const auto& r = j.at("rotation");
    const auto& s = j.at("scale");
    t.setPosition({p.at("x").get<float>(), p.at("y").get<float>(), p.at("z").get<float>()});
    t.setRotation({r.at("w").get<float>(), r.at("x").get<float>(), r.at("y").get<float>(),
                   r.at("z").get<float>()});
    t.setScale({s.at("x").get<float>(), s.at("y").get<float>(), s.at("z").get<float>()});
    return t;
}

json camera_to_json(const tracey::Camera& c) {
    auto p = c.position();
    auto r = c.rotation();
    return {
        {"position", {{"x", p.x}, {"y", p.y}, {"z", p.z}}},
        {"rotation", {{"w", r.w}, {"x", r.x}, {"y", r.y}, {"z", r.z}}},
        {"fov", c.fov()},
        {"near_plane", c.nearPlane()},
        {"far_plane", c.farPlane()},
        {"aspect_ratio", c.aspectRatio()},
    };
}

tracey::Camera camera_from_json(const json& j) {
    tracey::Camera c;
    const auto& p = j.at("position");
    const auto& r = j.at("rotation");
    c.setPosition({p.at("x").get<float>(), p.at("y").get<float>(), p.at("z").get<float>()});
    c.setRotation({r.at("w").get<float>(), r.at("x").get<float>(), r.at("y").get<float>(),
                   r.at("z").get<float>()});
    c.setFov(j.at("fov").get<float>());
    c.setNearPlane(j.at("near_plane").get<float>());
    c.setFarPlane(j.at("far_plane").get<float>());
    c.setAspectRatio(j.at("aspect_ratio").get<float>());
    return c;
}

json light_to_json(const tracey::Light& l) {
    return {
        {"type",          static_cast<int>(l.type)},
        {"color",         {{"x", l.color.x},        {"y", l.color.y},        {"z", l.color.z}}},
        {"intensity",     l.intensity},
        {"sky_color",     {{"x", l.skyColor.x},     {"y", l.skyColor.y},     {"z", l.skyColor.z}}},
        {"horizon_color", {{"x", l.horizonColor.x}, {"y", l.horizonColor.y}, {"z", l.horizonColor.z}}},
        {"ground_color",  {{"x", l.groundColor.x},  {"y", l.groundColor.y},  {"z", l.groundColor.z}}},
        {"hdri_path",     l.hdriPath},
        {"size",          {{"x", l.size.x}, {"y", l.size.y}}},
        {"radius",        l.radius},
    };
}

tracey::Light light_from_json(const json& j) {
    tracey::Light l;
    l.type      = static_cast<tracey::LightType>(j.value("type", 0));
    auto readV3 = [&](const char* key, tracey::Vec3& out) {
        if (!j.contains(key)) return;
        const auto& v = j.at(key);
        out.x = v.at("x").get<float>();
        out.y = v.at("y").get<float>();
        out.z = v.at("z").get<float>();
    };
    readV3("color",         l.color);
    readV3("sky_color",     l.skyColor);
    readV3("horizon_color", l.horizonColor);
    readV3("ground_color",  l.groundColor);
    l.intensity = j.value("intensity", 1.0f);
    l.radius    = j.value("radius",    0.0f);
    l.hdriPath  = j.value("hdri_path", std::string{});
    if (j.contains("size")) {
        const auto& s = j.at("size");
        l.size.x = s.at("x").get<float>();
        l.size.y = s.at("y").get<float>();
    }
    return l;
}

}  // namespace

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
