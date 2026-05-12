#pragma once
#include "../core/types.hpp"

namespace tracey
{
    // Houdini-style /obj-level light. Stored on Actor as an optional
    // component so the existing Scene::flatten + transform composition stack
    // applies for free (point-light position = actor world translation;
    // distant-light direction = actor rotation × -Z, mirroring how camera
    // forward is derived).
    //
    // v1 supports the two most useful types end-to-end:
    //   • Point: omnidirectional, intensity falls off as 1/r². World position
    //     comes from the actor's transform.
    //   • Distant: directional / "sun" — light is treated as infinitely far,
    //     so position is irrelevant; only the actor's rotation matters.
    //
    // Area / spot / environment land in a later pass. The shape is
    // forward-compatible: new types can extend the enum without breaking the
    // SSBO layout because each field stays a vec3 + float pair.
    enum class LightType : int
    {
        Point   = 0,
        Distant = 1,
    };

    struct Light
    {
        LightType type = LightType::Point;
        // Linear RGB (unbounded — pre-exposure). Renderer multiplies by
        // `intensity` at sample time.
        Vec3 color{1.0f, 1.0f, 1.0f};
        // Lumens-equivalent scalar. Treated as the radiant intensity factor
        // for now; a proper photometric model lands when units matter.
        float intensity = 1.0f;
    };
}
