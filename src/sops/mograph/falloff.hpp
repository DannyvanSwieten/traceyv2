// C4D-style falloff shared by every effector SOP: a spatial weight field
// the effector multiplies its per-point effect by. One parameter block
// (declared identically on every effector via declareFalloffParams /
// falloffParamSpecs) and one weight function.
//
// Shapes, on the normalized distance d:
//   Infinite — d = 0 (full weight everywhere)
//   Sphere   — d = |(p - center) / size| (per-component → ellipsoid)
//   Box      — d = max-component of |(p - center) / size| (Chebyshev)
//   Linear   — d = max(0, dot(p - center, normalize(axis)) / size.x)
//              (full weight at/behind center, ramps out along the axis)
// then  w = 1 - smoothstep(inner, 1, d); optional invert; × strength.
//
// Also hosts the per-point deterministic random + noise seed-shift helpers
// the random/noise effectors share (replicated from the TU-local versions
// in src/vops/nodes/noise_vops.cpp — those can't be linked against).

#pragma once

#include "../sop_node.hpp"
#include "../sop_registry.hpp"
#include "../../core/types.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace mograph
        {
            enum class FalloffShape
            {
                Infinite,
                Sphere,
                Box,
                Linear,
            };

            struct FalloffParams
            {
                FalloffShape shape = FalloffShape::Infinite;
                Vec3 center{0.0f};
                Vec3 size{1.0f};            // sphere: radii; box: half-extents; linear: size.x = ramp length
                Vec3 axis{1.0f, 0.0f, 0.0f}; // linear only
                float inner = 0.0f;          // 0..1 ratio of the full-weight core
                bool invert = false;
                float strength = 1.0f;       // 0..1 global multiplier
                bool weightToCd = false;     // debug: write the weight into Cd
            };

            inline FalloffShape falloffShapeFromString(const std::string &s)
            {
                if (s == "sphere") return FalloffShape::Sphere;
                if (s == "box")    return FalloffShape::Box;
                if (s == "linear") return FalloffShape::Linear;
                return FalloffShape::Infinite;
            }

            // Declare the shared parameter block on an effector (ctor helper).
            inline void declareFalloffParams(SopNode &n)
            {
                n.declareParam(Parameter::makeString("falloff_shape", "infinite"));
                n.declareParam(Parameter::makeVec3("falloff_center", Vec3(0.0f)));
                n.declareParam(Parameter::makeVec3("falloff_size", Vec3(1.0f)));
                n.declareParam(Parameter::makeVec3("falloff_axis", Vec3(1.0f, 0.0f, 0.0f)));
                n.declareParam(Parameter::makeFloat("falloff_inner", 0.0f));
                n.declareParam(Parameter::makeBool("falloff_invert", false));
                n.declareParam(Parameter::makeFloat("strength", 1.0f));
                n.declareParam(Parameter::makeBool("weight_to_cd", false));
            }

            // The matching catalog specs, spliced into every effector's
            // registration so the inspector renders the same falloff block
            // (shape dropdown, sliders) on each of them.
            inline std::vector<ParamSpec> falloffParamSpecs()
            {
                std::vector<ParamSpec> specs;
                specs.push_back({"falloff_shape", ParamType::String, "\"infinite\"",
                                 0.0, 0.0, 0.0,
                                 {"infinite", "sphere", "box", "linear"}});
                specs.push_back({"falloff_center", ParamType::Vec3, "[0, 0, 0]"});
                specs.push_back({"falloff_size", ParamType::Vec3, "[1, 1, 1]"});
                specs.push_back({"falloff_axis", ParamType::Vec3, "[1, 0, 0]"});
                specs.push_back({"falloff_inner", ParamType::Float, "0.0", 0.0, 1.0, 0.01});
                specs.push_back({"falloff_invert", ParamType::Bool, "false"});
                specs.push_back({"strength", ParamType::Float, "1.0", 0.0, 1.0, 0.01});
                specs.push_back({"weight_to_cd", ParamType::Bool, "false"});
                return specs;
            }

            // Sample the parameter block at `time` (everything numeric is
            // keyframable; the shape string is not).
            inline FalloffParams loadFalloffParamsAt(const SopNode &n, double time)
            {
                FalloffParams f;
                f.shape    = falloffShapeFromString(n.paramString("falloff_shape", "infinite"));
                f.center   = n.paramVec3At("falloff_center", time, Vec3(0.0f));
                f.size     = n.paramVec3At("falloff_size", time, Vec3(1.0f));
                f.axis     = n.paramVec3At("falloff_axis", time, Vec3(1.0f, 0.0f, 0.0f));
                f.inner    = n.paramFloatAt("falloff_inner", time, 0.0f);
                f.invert   = n.paramBool("falloff_invert", false);
                f.strength = n.paramFloatAt("strength", time, 1.0f);
                f.weightToCd = n.paramBool("weight_to_cd", false);
                return f;
            }

            inline float smoothstepf(float edge0, float edge1, float x)
            {
                const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f),
                                           0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            }

            inline float falloffWeight(const FalloffParams &f, const Vec3 &p)
            {
                float d = 0.0f;
                switch (f.shape)
                {
                case FalloffShape::Infinite:
                    d = 0.0f;
                    break;
                case FalloffShape::Sphere:
                {
                    const glm::vec3 q((p.x - f.center.x) / std::max(f.size.x, 1e-6f),
                                      (p.y - f.center.y) / std::max(f.size.y, 1e-6f),
                                      (p.z - f.center.z) / std::max(f.size.z, 1e-6f));
                    d = glm::length(q);
                    break;
                }
                case FalloffShape::Box:
                {
                    const float qx = std::abs(p.x - f.center.x) / std::max(f.size.x, 1e-6f);
                    const float qy = std::abs(p.y - f.center.y) / std::max(f.size.y, 1e-6f);
                    const float qz = std::abs(p.z - f.center.z) / std::max(f.size.z, 1e-6f);
                    d = std::max(qx, std::max(qy, qz));
                    break;
                }
                case FalloffShape::Linear:
                {
                    glm::vec3 axis(f.axis.x, f.axis.y, f.axis.z);
                    const float al = glm::length(axis);
                    if (al < 1e-6f) { d = 0.0f; break; }
                    axis /= al;
                    const glm::vec3 rel(p.x - f.center.x, p.y - f.center.y, p.z - f.center.z);
                    d = std::max(0.0f, glm::dot(rel, axis) / std::max(f.size.x, 1e-6f));
                    break;
                }
                }

                float w = 1.0f - smoothstepf(std::clamp(f.inner, 0.0f, 0.999f), 1.0f, d);
                if (f.invert) w = 1.0f - w;
                return w * std::clamp(f.strength, 0.0f, 1.0f);
            }

            // ── Per-point deterministic random ───────────────────────────
            // Wang-style integer hash; stable across cooks for a given
            // (index, seed) so random effectors don't flicker.
            inline uint32_t hashU32(uint32_t i, int seed)
            {
                uint32_t h = i * 0x9E3779B9u + static_cast<uint32_t>(seed) * 0x85EBCA6Bu;
                h = (h ^ 61u) ^ (h >> 16u);
                h *= 9u;
                h = h ^ (h >> 4u);
                h *= 0x27d4eb2du;
                h = h ^ (h >> 15u);
                return h;
            }

            inline float rand01(uint32_t i, int seed)
            {
                return static_cast<float>(hashU32(i, seed)) / 4294967296.0f;
            }

            // Three independent [-1, 1] floats per (index, seed).
            inline Vec3 rand3Signed(uint32_t i, int seed)
            {
                return Vec3(rand01(i, seed) * 2.0f - 1.0f,
                            rand01(i, seed + 1013) * 2.0f - 1.0f,
                            rand01(i, seed + 1031) * 2.0f - 1.0f);
            }

            // Decorrelate a noise field per seed by shifting the sample
            // position — mirrors the seedShift in vops/nodes/noise_vops.cpp.
            inline glm::vec3 seedShift(const glm::vec3 &p, int seed)
            {
                const float s = static_cast<float>(seed);
                return p + glm::vec3(s * 17.131f, s * 31.711f, s * 53.917f);
            }
        } // namespace mograph
    } // namespace sops
} // namespace tracey
