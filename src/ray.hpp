#pragma once
#include "types.hpp"

namespace tracey
{
    struct Ray
    {
        Vec3 origin;
        Vec3 direction;
    };

    using RayFlags = size_t;
    static constexpr RayFlags RAY_FLAG_NONE = 0;

    static constexpr RayFlags RAY_FLAG_CULL_BACK_FACES = 1 << 0;
    static constexpr RayFlags RAY_FLAG_CULL_FRONT_FACES = 1 << 1;
    static constexpr RayFlags RAY_FLAG_CULL_ALL_FACES = RAY_FLAG_CULL_BACK_FACES | RAY_FLAG_CULL_FRONT_FACES;

    static constexpr RayFlags RAY_FLAG_TERMINATE_ON_FIRST_HIT = 1 << 3;
    static constexpr RayFlags RAY_FLAG_OPAQUE = 1 << 4;
}