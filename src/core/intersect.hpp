#pragma once
#include <tuple>
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
    inline bool intersectTriangle(const Ray &ray, const Vec3 &v0, const Vec3 &v1, const Vec3 &v2, float &tOut, float &uOut, float &vOut)
    {
        const float EPSILON = 1e-8f;
        Vec3 edge1 = v1 - v0;
        Vec3 edge2 = v2 - v0;
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

    inline std::tuple<tracey::Vec3, tracey::Vec3> transformAABB(const tracey::Mat4 &M,
                                                                const tracey::Vec3 &localMin,
                                                                const tracey::Vec3 &localMax)
    {
        // Center-extents form of the local AABB
        tracey::Vec3 center = 0.5f * (localMin + localMax);
        tracey::Vec3 half = 0.5f * (localMax - localMin);

        // Transform the center
        tracey::Vec3 worldCenter = M * tracey::Vec4(center, 1.0f);

        // Take the linear (3x3) part of M (rotation + scale)
        tracey::Mat3 R = tracey::Mat3(M);

        // Absolute value matrix so we get conservative extents
        glm::mat3 A;
        A[0] = glm::abs(R[0]);
        A[1] = glm::abs(R[1]);
        A[2] = glm::abs(R[2]);

        // New half-extents
        tracey::Vec3 worldHalf = A * half;

        return {
            worldCenter - worldHalf,
            worldCenter + worldHalf};
    }
}