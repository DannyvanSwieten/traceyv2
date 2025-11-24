#pragma once
#include "types.hpp"

namespace tracey
{
    struct Hit
    {
        Vec3 position;        // Position of the hit
        float t;              // Distance along the ray
        float u, v;           // Barycentric coordinates
        uint32_t instanceId;  // ID of the instance hit
        uint32_t primitiveId; // Index of the intersected primitive
    };
}