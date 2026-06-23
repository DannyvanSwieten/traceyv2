#pragma once
#include <tuple>
#include <algorithm>
#include "ray.hpp"
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
namespace tracey
{

    // Ray/AABB slab test — the hottest function in the CPU tracer (one call per
    // visited BVH child). NEON does all three axes in a single 4-wide pass.
    //
    // The 4-wide loads over-read lane 3 into the next struct member
    // (BVHNode::firstChildOrPrim / primCountAndType for the bounds; Ray::direction
    // / time for the ray) — always in-bounds because every caller passes BVHNode
    // bounds + a Ray (verified). Lane 3 is then overwritten with minT/maxT, so the
    // horizontal reduce folds the caller's [minT,maxT] clamp in for free and the
    // garbage never reaches the result. vminq/vmaxq match glm::min/max for finite
    // inputs (verified bit-identical against the scalar path via pt_backend_compare).
    inline bool intersectAABB(const Ray &r, const tracey::Vec3 &bmin, const tracey::Vec3 &bmax, float minT, float maxT,
                              float &tEnter, float &tExit)
    {
#if defined(__ARM_NEON)
        const float32x4_t o   = vld1q_f32(&r.origin.x);
        const float32x4_t inv = vld1q_f32(&r.invDirection.x);
        const float32x4_t t0  = vmulq_f32(vsubq_f32(vld1q_f32(&bmin.x), o), inv);
        const float32x4_t t1  = vmulq_f32(vsubq_f32(vld1q_f32(&bmax.x), o), inv);
        float32x4_t tmn = vminq_f32(t0, t1);
        float32x4_t tmx = vmaxq_f32(t0, t1);
        tmn = vsetq_lane_f32(minT, tmn, 3);  // fold the [minT,maxT] clamp into the
        tmx = vsetq_lane_f32(maxT, tmx, 3);  // horizontal reduce (lane 3 was garbage)
        tEnter = vmaxvq_f32(tmn);
        tExit = vminvq_f32(tmx);
        return tExit >= tEnter;
#else
        glm::vec3 t0 = (bmin - r.origin) * r.invDirection;
        glm::vec3 t1 = (bmax - r.origin) * r.invDirection;

        glm::vec3 tMin = glm::min(t0, t1);
        glm::vec3 tMax = glm::max(t0, t1);

        tEnter = std::max({tMin.x, tMin.y, tMin.z, minT});
        tExit = std::min({tMax.x, tMax.y, tMax.z, maxT});

        return tExit >= tEnter;
#endif
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

    inline std::tuple<tracey::Vec3, tracey::Vec3> transformAABB(const Mat4 &M,
                                                                const tracey::Vec3 &localMin,
                                                                const tracey::Vec3 &localMax)
    {
        // Center-extents form of the local AABB
        tracey::Vec3 center = 0.5f * (localMin + localMax);
        tracey::Vec3 half = 0.5f * (localMax - localMin);

        // Transform the center (M is Vk-style row-major 3x4)
        tracey::Vec3 worldCenter = transformPoint(M, center);

        // M is a glsl style mat4 (column-major), so we extract rows like this
        tracey::Vec3 r0(M[0][0], M[1][0], M[2][0]);
        tracey::Vec3 r1(M[0][1], M[1][1], M[2][1]);
        tracey::Vec3 r2(M[0][2], M[1][2], M[2][2]);

        // New conservative half-extents: |R| * half, but using row-major rows
        tracey::Vec3 worldHalf(
            std::abs(r0.x) * half.x + std::abs(r0.y) * half.y + std::abs(r0.z) * half.z,
            std::abs(r1.x) * half.x + std::abs(r1.y) * half.y + std::abs(r1.z) * half.z,
            std::abs(r2.x) * half.x + std::abs(r2.y) * half.y + std::abs(r2.z) * half.z);

        return {worldCenter - worldHalf, worldCenter + worldHalf};
    }
}