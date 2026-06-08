#pragma once
#include <string>
#include "../core/types.hpp"

namespace tracey
{
    // Scene-level light component. Stored on Actor as an optional component so
    // the existing Scene::flatten + transform composition stack applies for
    // free (Point.position = actor world translation; Distant.direction =
    // actor rotation × -Z, mirroring how camera forward is derived; Dome is
    // transform-independent; Area's centre + orientation come from the
    // actor's transform, with `size` giving the local-space rectangle).
    //
    // v1 supports four types:
    //   • Point   — omnidirectional, intensity falls off as 1/(d² + radius²).
    //               radius=0 is a perfect point; positive values soften the
    //               near-field falloff (avoids the harsh "headlight" look).
    //   • Distant — directional / "sun" — position-independent.
    //   • Dome    — environment IBL. Procedural sky/horizon/ground gradient
    //               drives both the rasterizer's IBL term and the path
    //               tracer's miss shader. HDRI override path is reserved
    //               (loader lands in a follow-up); empty = procedural.
    //   • Area    — finite rectangle (size.x × size.y in local XY plane,
    //               normal = local +Z). Two-sided. PT samples the surface
    //               for soft shadows; rasterizer approximates as a Distant
    //               along the area's -Z because per-fragment area
    //               integration is too costly for the viewport.
    //
    // SSBO layout (5 × vec4 = 80 bytes — see GPULight in scene_compiler.hpp)
    // is forward-compatible: new types can be added without breaking the
    // wire, and unused fields cost zero at draw time.
    enum class LightType : int
    {
        Point   = 0,
        Distant = 1,
        Dome    = 2,
        Area    = 3,
    };

    struct Light
    {
        LightType type = LightType::Point;

        // Linear RGB (unbounded — pre-exposure). Renderer multiplies by
        // `intensity` at sample time. For Dome lights this is the "tint"
        // multiplier; the gradient colours below carry the actual hemisphere
        // shape and `color` modulates them.
        Vec3 color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;

        // Dome-specific procedural sky gradient. Saturated daytime-sky
        // defaults — picked so the colours survive the renderer's Reinhard
        // tonemap (the previous pastel-grey defaults collapsed to a near-
        // monochrome sky once `c/(c+1)` mapped channels close to 1.0 into
        // the [0.4, 0.5] midtone band). Blue zenith, warm horizon, dark
        // earth ground; intensity multiplies on top of these for a
        // sunny-vs-overcast feel.
        Vec3 skyColor    {0.20f, 0.45f, 1.00f};
        Vec3 horizonColor{1.00f, 0.65f, 0.30f};
        Vec3 groundColor {0.10f, 0.08f, 0.06f};

        // Optional HDRI override for Dome lights. Empty = use the procedural
        // gradient above. Resolution + prefilter pipeline land in a
        // follow-up; today this field round-trips through save/load so
        // user-authored projects keep the value while we build the loader.
        std::string hdriPath;

        // Area-light extent in the actor's local XY plane (rectangle
        // centred at origin, normal = +Z before the actor transform).
        Vec2 size{1.0f, 1.0f};

        // Point-light soft radius (metres). 0.0 = perfect point;
        // positive values soften 1/d² near the source so a light dragged
        // through a fragment doesn't blow out the colour.
        float radius = 0.0f;
    };
}
