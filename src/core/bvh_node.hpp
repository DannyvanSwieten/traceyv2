#pragma once
#include "types.hpp"
namespace tracey
{
    static constexpr uint8_t BVH_LEAF_TYPE_TRIANGLES = 0;
    static constexpr uint8_t BVH_LEAF_TYPE_PROCEDURAL = 1;

    struct BVHNode
    {
        Vec3 boundsMin;
        uint32_t firstChildOrPrim;

        Vec3 boundsMax;
        uint32_t primCountAndType; // packed (leafType << 24) | primitiveCount

        // Convention:
        // if primCount > 0 => leaf
        //   firstChildOrPrim = index into primitive index array
        // else (primCount == 0) => interior
        //   firstChildOrPrim = index of left child
        //   right child = left child + 1

        // Make sure padding/layout is understood if you share it with GLSL.
    };
}