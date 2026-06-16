// Rotation-minimizing frames (RMF) along an ordered point list — the shared
// math behind the curve generators, Resample, and Sweep. A "curve" in tracey
// is just an ordered point cloud (point index = path order) plus a Detail int
// `closed` flag; this header turns that point order into a smooth per-point
// frame and bakes it into the standard `N` (frame up) + `orient` (Vec4 wxyz
// quaternion) point attributes the cloners already consume.
//
// Frame convention matches orient_util.hpp / orientFromNormal:
//   local +Z = tangent T   (so a clone's forward axis aims along the path)
//   local +Y = normal  N   (frame "up")
//   local +X = bitangent B (frame "right")
// The basis is right-handed (B × N = T), which quat_cast requires.
//
// The frames are computed with the double-reflection method (Wang et al.,
// "Computation of Rotation Minimizing Frames", 2008) — O(n), no trig in the
// transport step, and far less twist than a fixed-up frame (which flips when
// the tangent aligns with the up axis). For closed curves the loop's residual
// twist (holonomy) is distributed evenly so the seam closes smoothly.

#pragma once

#include "../../core/types.hpp"
#include "../../geometry/geometry.hpp"
#include "orient_util.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace mograph
        {
            struct CurveFrame
            {
                glm::vec3 T{0.0f, 0.0f, 1.0f}; // tangent      -> local +Z
                glm::vec3 N{0.0f, 1.0f, 0.0f}; // normal / up  -> local +Y
                glm::vec3 B{1.0f, 0.0f, 0.0f}; // bitangent     -> local +X
            };

            namespace detail
            {
                inline glm::vec3 safeNormalize(const glm::vec3 &v, const glm::vec3 &fallback)
                {
                    const float l2 = glm::dot(v, v);
                    return l2 > 1e-12f ? v * (1.0f / std::sqrt(l2)) : fallback;
                }

                // Orthonormal right-handed (B, N, T) from a tangent and an
                // approximate up. Mirrors orientFromNormal: B = cross(up, T),
                // N = cross(T, B). Both come out unit and mutually perpendicular.
                inline void frameFromTangentUp(const glm::vec3 &T, const glm::vec3 &up,
                                               const glm::vec3 &fallbackB,
                                               glm::vec3 &B, glm::vec3 &N)
                {
                    B = safeNormalize(glm::cross(up, T), fallbackB);
                    N = glm::cross(T, B);
                }
            }

            inline std::vector<CurveFrame> computeCurveFrames(const std::vector<Vec3> &P,
                                                              bool closed)
            {
                const size_t n = P.size();
                std::vector<CurveFrame> frames(n);
                if (n <= 1) return frames; // identity default for empty/single

                // ── tangents (central diff interior; one-sided open ends;
                //    wrap for closed) ──
                std::vector<glm::vec3> T(n);
                for (size_t i = 0; i < n; ++i)
                {
                    glm::vec3 d;
                    if (closed)
                        d = P[(i + 1) % n] - P[(i + n - 1) % n];
                    else if (i == 0)
                        d = P[1] - P[0];
                    else if (i == n - 1)
                        d = P[n - 1] - P[n - 2];
                    else
                        d = P[i + 1] - P[i - 1];
                    T[i] = detail::safeNormalize(d, glm::vec3(0.0f, 0.0f, 1.0f));
                }

                // ── seed frame (pick an up that isn't parallel to T[0]) ──
                const glm::vec3 up = std::fabs(T[0].y) < 0.999f
                                         ? glm::vec3(0.0f, 1.0f, 0.0f)
                                         : glm::vec3(1.0f, 0.0f, 0.0f);
                glm::vec3 B0, N0;
                detail::frameFromTangentUp(T[0], up, glm::vec3(1.0f, 0.0f, 0.0f), B0, N0);
                frames[0] = {T[0], N0, B0};

                // ── double-reflection transport of the normal along each segment ──
                auto transportNormal = [](const glm::vec3 &xi, const glm::vec3 &xj,
                                          const glm::vec3 &ti, const glm::vec3 &tj,
                                          const glm::vec3 &ni) -> glm::vec3 {
                    const glm::vec3 v1 = xj - xi;
                    const float c1 = glm::dot(v1, v1);
                    if (c1 < 1e-12f) return ni; // coincident points: carry frame
                    const glm::vec3 rL = ni - (2.0f / c1) * glm::dot(v1, ni) * v1;
                    const glm::vec3 tL = ti - (2.0f / c1) * glm::dot(v1, ti) * v1;
                    const glm::vec3 v2 = tj - tL;
                    const float c2 = glm::dot(v2, v2);
                    if (c2 < 1e-12f) return rL; // tangents already aligned
                    return rL - (2.0f / c2) * glm::dot(v2, rL) * v2;
                };

                const size_t steps = closed ? n : n - 1;
                glm::vec3 Ncur = N0;
                glm::vec3 N0prime = N0; // transported normal arriving back at point 0 (closed)
                for (size_t s = 0; s < steps; ++s)
                {
                    const size_t i = s;
                    const size_t j = (s + 1) % n;
                    const glm::vec3 rNext = transportNormal(P[i], P[j], T[i], T[j], Ncur);
                    glm::vec3 Bn, Nn;
                    detail::frameFromTangentUp(T[j], rNext, frames[i].B, Bn, Nn);
                    Ncur = Nn;
                    if (closed && j == 0)
                        N0prime = Nn; // seam: don't overwrite the seed frame
                    else
                        frames[j] = {T[j], Nn, Bn};
                }

                // ── closed seam: distribute the holonomy twist evenly ──
                if (closed && n >= 3)
                {
                    const float theta = std::atan2(
                        glm::dot(glm::cross(N0, N0prime), T[0]),
                        glm::dot(N0, N0prime));
                    for (size_t i = 0; i < n; ++i)
                    {
                        const float ang = -theta * (static_cast<float>(i) / static_cast<float>(n));
                        const glm::quat rot = glm::angleAxis(ang, frames[i].T);
                        frames[i].N = rot * frames[i].N;
                        frames[i].B = rot * frames[i].B;
                    }
                }
                return frames;
            }

            // wxyz quaternion for a frame: columns X'=B, Y'=N, Z'=T.
            inline Vec4 frameToOrient(const CurveFrame &f)
            {
                const glm::mat3 R(f.B, f.N, f.T);
                return wxyzFromQuat(glm::normalize(glm::quat_cast(R)));
            }

            // Compute frames from `geo`'s ordered points and write the `N`
            // (point Vec3) and `orient` (point Vec4 wxyz) attributes, adding
            // them if absent. Call after the points + P are populated.
            inline void writeCurveFrames(Geometry &geo, bool closed)
            {
                const auto &P = geo.positions();
                const auto frames = computeCurveFrames(P, closed);
                auto *N = geo.points().get<Vec3>("N");
                if (!N) N = geo.points().add<Vec3>("N", Vec3(0.0f, 1.0f, 0.0f));
                auto *orient = geo.points().get<Vec4>("orient");
                if (!orient) orient = geo.points().add<Vec4>("orient", identityWxyz());
                auto &Nd = N->data();
                auto &Od = orient->data();
                for (size_t i = 0; i < frames.size(); ++i)
                {
                    Nd[i] = frames[i].N;
                    Od[i] = frameToOrient(frames[i]);
                }
            }

            // Convenience: stamp the `closed` flag as a Detail int attribute
            // (the curve-topology contract consumed by Resample / Sweep).
            inline void setCurveClosed(Geometry &geo, bool closed)
            {
                auto *c = geo.detail().get<int>("closed");
                if (!c) c = geo.detail().add<int>("closed", closed ? 1 : 0);
                if (!c->data().empty()) c->data()[0] = closed ? 1 : 0;
            }

            inline bool curveClosed(const Geometry &geo)
            {
                const auto *c = geo.detail().get<int>("closed");
                return c && !c->data().empty() && c->data()[0] != 0;
            }
        } // namespace mograph
    } // namespace sops
} // namespace tracey
