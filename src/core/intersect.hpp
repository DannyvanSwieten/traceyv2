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
    // Watertight ray/triangle intersection — Woop, Benthin & Wald, "Watertight
    // Ray/Triangle Intersection" (JCGT 2013). Replaces Möller-Trumbore's strict
    // u,v ∈ [0,1] test, which leaks: at a shared edge between two triangles,
    // floating-point error can make *both* triangles reject the ray, so it slips
    // through the crack in the mesh. Bounce/shadow rays that leak escape an
    // enclosed scene and pick up the bright environment, washing out shadows.
    // This formulation computes the shared-edge sign identically for both
    // triangles (with a double-precision fallback exactly on an edge), so a ray
    // can neither slip between adjacent triangles nor be counted by both — the
    // watertight behaviour the Metal hardware ray tracer already has.
    //
    // Two-sided (no back-face cull): the renderer shadows and bounces off both
    // faces. Same signature as before — v1/v2 are reconstructed from the edges,
    // and (uOut,vOut) keep Möller-Trumbore's convention (barycentric weights of
    // v1 and v2), so callers and attribute interpolation are unchanged.
    inline bool intersectTriangle(const Ray &ray, const Vec3 &v0, const Vec3 &edge1, const Vec3 &edge2, float &tOut, float &uOut, float &vOut)
    {
        const float EPSILON = 1e-8f;
        const Vec3 v1 = v0 + edge1;
        const Vec3 v2 = v0 + edge2;
        const Vec3 &dir = ray.direction;

        // Per-ray shear/permutation. kz is the axis of greatest |direction|;
        // kx,ky cycle after it (swapped when dir[kz] < 0 to preserve winding so
        // the edge-sign test is consistent across the shared edge). Cheap enough
        // to recompute per triangle. dir[kz] is the largest-magnitude component,
        // so it is non-zero for any real (non-degenerate) ray.
        const Vec3 ad(std::abs(dir.x), std::abs(dir.y), std::abs(dir.z));
        int kz = 0;
        float amax = ad.x;
        if (ad.y > amax) { kz = 1; amax = ad.y; }
        if (ad.z > amax) { kz = 2; amax = ad.z; }
        if (!(amax > 0.0f))
            return false; // degenerate ray: zero-length or NaN direction
        int kx = kz + 1; if (kx == 3) kx = 0;
        int ky = kx + 1; if (ky == 3) ky = 0;
        if (dir[kz] < 0.0f) { const int tmp = kx; kx = ky; ky = tmp; }

        const float Sx = dir[kx] / dir[kz];
        const float Sy = dir[ky] / dir[kz];
        const float Sz = 1.0f / dir[kz];

        // Vertices translated to ray origin, then sheared into the (kx,ky) plane.
        const Vec3 A = v0 - ray.origin;
        const Vec3 B = v1 - ray.origin;
        const Vec3 C = v2 - ray.origin;
        const float Ax = A[kx] - Sx * A[kz];
        const float Ay = A[ky] - Sy * A[kz];
        const float Bx = B[kx] - Sx * B[kz];
        const float By = B[ky] - Sy * B[kz];
        const float Cx = C[kx] - Sx * C[kz];
        const float Cy = C[ky] - Sy * C[kz];

        // Scaled barycentrics (edge functions). U,V,W are the weights of v0,v1,v2.
        float U = Cx * By - Cy * Bx;
        float V = Ax * Cy - Ay * Cx;
        float W = Bx * Ay - By * Ax;

        // Fall back to double precision exactly on an edge (any function == 0).
        // This is what makes the test watertight: the shared edge resolves to the
        // same sign for both adjacent triangles instead of a fp coin-flip.
        if (U == 0.0f || V == 0.0f || W == 0.0f)
        {
            U = static_cast<float>(static_cast<double>(Cx) * static_cast<double>(By) - static_cast<double>(Cy) * static_cast<double>(Bx));
            V = static_cast<float>(static_cast<double>(Ax) * static_cast<double>(Cy) - static_cast<double>(Ay) * static_cast<double>(Cx));
            W = static_cast<float>(static_cast<double>(Bx) * static_cast<double>(Ay) - static_cast<double>(By) * static_cast<double>(Ax));
        }

        // Two-sided edge test: a hit requires U,V,W to all share a sign (zeros,
        // i.e. edges/vertices, are allowed). Reject only when the signs disagree.
        if ((U < 0.0f || V < 0.0f || W < 0.0f) && (U > 0.0f || V > 0.0f || W > 0.0f))
            return false;

        const float det = U + V + W;
        if (det == 0.0f)
            return false; // ray coplanar with the triangle

        // Interpolate the sheared z of each vertex → hit distance. The shear maps
        // the ray direction to +z, so the interpolated z divided by det is t.
        const float Az = Sz * A[kz];
        const float Bz = Sz * B[kz];
        const float Cz = Sz * C[kz];
        const float T = U * Az + V * Bz + W * Cz;

        const float rcpDet = 1.0f / det;
        const float t = T * rcpDet; // det's sign cancels: t > 0 for forward hits
        // Written as !(t > EPSILON) (not t <= EPSILON) so a NaN t is rejected —
        // matches Möller-Trumbore's old final gate. A NaN/Inf hit must never be
        // reported: it would slip past the caller's closestT cull and leave the
        // BVH traversal unable to tighten, deepening the stack.
        if (!(t > EPSILON))
            return false;

        tOut = t;
        uOut = V * rcpDet; // barycentric weight of v1 (Möller-Trumbore's u)
        vOut = W * rcpDet; // barycentric weight of v2 (Möller-Trumbore's v)
        return true;
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