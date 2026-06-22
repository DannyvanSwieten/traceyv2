// Shared JSON ⇄ engine-type converters for the native editor host.
// Used by editor_server.cpp (IPC payloads) and scene_state.cpp
// (.tracey save/load) so the wire format can't drift between the two.

#pragma once

#include "core/types.hpp"
#include "scene/camera.hpp"
#include "scene/light.hpp"
#include "scene/transform.hpp"

#include <json.hpp>

#include <string>

namespace tracey_editor {

inline nlohmann::json vec3_to_json(const tracey::Vec3& v) {
    return {{"x", v.x}, {"y", v.y}, {"z", v.z}};
}

inline tracey::Vec3 vec3_from_json(const nlohmann::json& j) {
    return {j.at("x").get<float>(), j.at("y").get<float>(), j.at("z").get<float>()};
}

inline nlohmann::json quat_to_json(const tracey::Quaternion& q) {
    return {{"w", q.w}, {"x", q.x}, {"y", q.y}, {"z", q.z}};
}

inline tracey::Quaternion quat_from_json(const nlohmann::json& j) {
    return {j.at("w").get<float>(), j.at("x").get<float>(), j.at("y").get<float>(),
            j.at("z").get<float>()};
}

inline nlohmann::json transform_to_json(const tracey::Transform& t) {
    return {
        {"position", vec3_to_json(t.position())},
        {"rotation", quat_to_json(t.rotation())},
        {"scale", vec3_to_json(t.scale())},
    };
}

inline tracey::Transform transform_from_json(const nlohmann::json& j) {
    tracey::Transform t;
    t.setPosition(vec3_from_json(j.at("position")));
    t.setRotation(quat_from_json(j.at("rotation")));
    t.setScale(vec3_from_json(j.at("scale")));
    return t;
}

inline nlohmann::json camera_to_json(const tracey::Camera& c) {
    return {
        {"position", vec3_to_json(c.position())},
        {"rotation", quat_to_json(c.rotation())},
        {"fov", c.fov()},
        {"near_plane", c.nearPlane()},
        {"far_plane", c.farPlane()},
        {"aspect_ratio", c.aspectRatio()},
        {"aperture", c.aperture()},
        {"focal_distance", c.focalDistance()},
        {"shutter", c.shutter()},
    };
}

inline tracey::Camera camera_from_json(const nlohmann::json& j) {
    tracey::Camera c;
    c.setPosition(vec3_from_json(j.at("position")));
    c.setRotation(quat_from_json(j.at("rotation")));
    c.setFov(j.at("fov").get<float>());
    c.setNearPlane(j.at("near_plane").get<float>());
    c.setFarPlane(j.at("far_plane").get<float>());
    c.setAspectRatio(j.at("aspect_ratio").get<float>());
    // DOF — default to 0 (pinhole) so scenes saved before R4 still load.
    c.setAperture(j.value("aperture", 0.0f));
    c.setFocalDistance(j.value("focal_distance", 5.0f));
    c.setShutter(j.value("shutter", 0.0f));
    return c;
}

inline nlohmann::json light_to_json(const tracey::Light& l) {
    return {
        {"type",          static_cast<int>(l.type)},
        {"color",         vec3_to_json(l.color)},
        {"intensity",     l.intensity},
        {"sky_color",     vec3_to_json(l.skyColor)},
        {"horizon_color", vec3_to_json(l.horizonColor)},
        {"ground_color",  vec3_to_json(l.groundColor)},
        {"hdri_path",     l.hdriPath},
        {"size",          {{"x", l.size.x}, {"y", l.size.y}}},
        {"radius",        l.radius},
    };
}

inline tracey::Light light_from_json(const nlohmann::json& j) {
    tracey::Light l;
    l.type = static_cast<tracey::LightType>(j.value("type", 0));
    auto readV3 = [&](const char* key, tracey::Vec3& out) {
        if (!j.contains(key)) return;
        out = vec3_from_json(j.at(key));
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

}  // namespace tracey_editor
