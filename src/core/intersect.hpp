#pragma once
#include <tuple>
#include <algorithm>
#include "ray.hpp"
namespace tracey
{

    inline bool intersectAABB(const Ray &r, const tracey::Vec3 &bmin, const tracey::Vec3 &bmax, float minT, float maxT,
                              float &tEnter, float &tExit)
    {
        glm::vec3 t0 = (bmin - r.origin) * r.invDirection;
        glm::vec3 t1 = (bmax - r.origin) * r.invDirection;

        glm::vec3 tMin = glm::min(t0, t1);
        glm::vec3 tMax = glm::max(t0, t1);

        tEnter = std::max({tMin.x, tMin.y, tMin.z, minT});
        tExit = std::min({tMax.x, tMax.y, tMax.z, maxT});

        return tExit >= tEnter;
    }
    inline bool intersectTriangle(const Ray &ray, const Vec3 &v0, const Vec3 &edge1, const Vec3 &edge2, float &tOut, float &uOut, float &vOut)
    {
        const float EPSILON = 1e-8f;
        Vec3 h = glm::cross(ray.direction, edge2);
        float a = glm::dot(edge1, h);
        if (a > -EPSILON && a < EPSILON)
            return false; // This ray is parallel to this triangle.

        float f = 1.0f / a;
        Vec3 s = ray.origin - v0;
        float u = f * glm::dot(s, h);
        if (u < 0.0f || u > 1.0f)
            return false;

        Vec3 q = glm::cross(s, edge1);
        float v = f * glm::dot(ray.direction, q);
        if (v < 0.0f || u + v > 1.0f)
            return false;

        // At this stage we can compute t to find out where the intersection point is on the line.
        float t = f * glm::dot(edge2, q);
        if (t > EPSILON) // ray intersection
        {
            tOut = t;
            uOut = u;
            vOut = v;
            return true;
        }
        else // This means that there is a line intersection but not a ray intersection.
            return false;
    }

    inline std::tuple<tracey::Vec3, tracey::Vec3> transformAABB(const float M[3][4],
                                                                const tracey::Vec3 &localMin,
                                                                const tracey::Vec3 &localMax)
    {
        // Center-extents form of the local AABB
        tracey::Vec3 center = 0.5f * (localMin + localMax);
        tracey::Vec3 half = 0.5f * (localMax - localMin);

        // Transform the center (M is Vk-style row-major 3x4)
        tracey::Vec3 worldCenter = transformPoint(M, center);

        // Rows of the linear (3x3) part of M (rotation + scale)
        tracey::Vec3 r0(M[0][0], M[0][1], M[0][2]);
        tracey::Vec3 r1(M[1][0], M[1][1], M[1][2]);
        tracey::Vec3 r2(M[2][0], M[2][1], M[2][2]);

        // New conservative half-extents: |R| * half, but using row-major rows
        tracey::Vec3 worldHalf(
            std::abs(r0.x) * half.x + std::abs(r0.y) * half.y + std::abs(r0.z) * half.z,
            std::abs(r1.x) * half.x + std::abs(r1.y) * half.y + std::abs(r1.z) * half.z,
            std::abs(r2.x) * half.x + std::abs(r2.y) * half.y + std::abs(r2.z) * half.z);

        return {worldCenter - worldHalf, worldCenter + worldHalf};
    }
}