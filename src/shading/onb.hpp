#pragma once
#include "../core/types.hpp"

namespace tracey
{
    struct OrthogonalBasis
    {
        Vec3 tangent;
        Vec3 bitangent;
        Vec3 normal;

        static OrthogonalBasis fromNormal(const Vec3 &n)
        {
            Vec3 tangent;
            if (std::abs(n.x) > std::abs(n.z))
                tangent = glm::normalize(Vec3(-n.y, n.x, 0.0f));
            else
                tangent = glm::normalize(Vec3(0.0f, -n.z, n.y));
            Vec3 bitangent = glm::cross(n, tangent);
            return {tangent, bitangent, n};
        }

        Vec3 toWorld(const Vec3 &v) const
        {
            return v.x * tangent + v.y * bitangent + v.z * normal;
        }
    };
}