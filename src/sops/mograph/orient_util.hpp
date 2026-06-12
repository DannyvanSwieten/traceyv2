// Shared orientation math for the cloner/effector pipeline. One home for
// the frame/quaternion conversions that were previously duplicated across
// copy_to_points_sop.cpp, sop_graph.cpp's instance emit branches, and
// transform_sop.cpp. Per-clone rotation travels as a wxyz quaternion in a
// Vec4 (the `orient` point attribute and EmittedActor::InstanceEntry both
// use that layout).

#pragma once

#include "../../core/types.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace tracey
{
    namespace sops
    {
        namespace mograph
        {
            // Build the rotation matrix that maps the stamp's local +Z to the
            // direction `n`, using `up` as the up reference. Returns identity
            // when `n` is near-zero or parallel to `up` (no well-defined
            // frame; fall back to no rotation so flat ground points "just
            // work" with up=+Y, N=+Y).
            inline glm::mat3 orientFromNormal(const Vec3 &n,
                                              const Vec3 &up = Vec3(0.0f, 1.0f, 0.0f))
            {
                const float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
                if (len2 < 1e-12f) return glm::mat3(1.0f);
                const float invLen = 1.0f / std::sqrt(len2);
                glm::vec3 forward(n.x * invLen, n.y * invLen, n.z * invLen);

                glm::vec3 upRef(up.x, up.y, up.z);
                glm::vec3 right = glm::cross(upRef, forward);
                const float r2 = glm::dot(right, right);
                if (r2 < 1e-12f) return glm::mat3(1.0f);  // forward parallel to up
                right = right * (1.0f / std::sqrt(r2));
                glm::vec3 newUp = glm::cross(forward, right);

                // Columns are the basis vectors of the rotated frame:
                // X' = right, Y' = newUp, Z' = forward.
                return glm::mat3(right, newUp, forward);
            }

            // Compose an euler-deg vec3 (Houdini convention: Rx then Ry then
            // Rz, i.e. q = qz * qy * qx in glm) into a wxyz quaternion Vec4.
            inline Vec4 eulerDegToQuatWxyz(const Vec3 &deg)
            {
                constexpr float kDeg2Rad = 3.1415926535f / 180.0f;
                const Vec3 rad = deg * kDeg2Rad;
                glm::quat qx = glm::angleAxis(rad.x, glm::vec3(1, 0, 0));
                glm::quat qy = glm::angleAxis(rad.y, glm::vec3(0, 1, 0));
                glm::quat qz = glm::angleAxis(rad.z, glm::vec3(0, 0, 1));
                glm::quat q  = qz * qy * qx;
                return Vec4(q.w, q.x, q.y, q.z);
            }

            // Vec4 (wxyz) ⇄ glm::quat, normalizing on the way in so attribute
            // data that drifted from unit length can't shear the clones.
            inline glm::quat quatFromWxyz(const Vec4 &q)
            {
                glm::quat out(q.x, q.y, q.z, q.w);  // glm ctor is (w, x, y, z)
                const float len = glm::length(out);
                if (len < 1e-12f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                return out * (1.0f / len);
            }

            inline Vec4 wxyzFromQuat(const glm::quat &q)
            {
                return Vec4(q.w, q.x, q.y, q.z);
            }

            inline glm::mat3 mat3FromWxyz(const Vec4 &q)
            {
                return glm::mat3_cast(quatFromWxyz(q));
            }

            // The wxyz identity used to seed a fresh `orient` attribute.
            inline Vec4 identityWxyz() { return Vec4(1.0f, 0.0f, 0.0f, 0.0f); }
        } // namespace mograph
    } // namespace sops
} // namespace tracey
