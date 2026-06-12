// Base class for the MoGraph effector SOPs (plain / random / noise / step).
//
// An effector is a point-stream modifier: it reads the incoming template
// point cloud and modulates per-clone position (P), rotation (`orient`
// Vec4 wxyz quaternion), scale (pscale) and colour (Cd) — each weighted by
// the shared C4D-style falloff (falloff.hpp). The cloners downstream
// consume the result: copy_to_points / instance / instance_vop all honour
// `orient` over the N-derived frame.
//
// `orient` is created LAZILY: only when the effector actually applies a
// rotation (or the attribute already exists upstream). An unconditional
// orient would force the copy_to_points GPU dispatcher onto its CPU
// fallback even for rotation-free effector chains.

#pragma once

#include "falloff.hpp"
#include "orient_util.hpp"

#include "../sop_node.hpp"
#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <glm/gtc/quaternion.hpp>

#include <functional>

namespace tracey
{
    namespace sops
    {
        namespace mograph
        {
            class EffectorSopBase : public SopNode
            {
            public:
                explicit EffectorSopBase(size_t uid) : SopNode(uid)
                {
                    declareFalloffParams(*this);
                }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                // Effectors are time-aware (params are routinely keyframed);
                // subclasses implement cookAt and the timeless path delegates.
                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    return cookAt(inputs, 0.0);
                }

            protected:
                // Mutable per-point views handed to the per-effector lambda.
                struct PointCtx
                {
                    size_t index = 0;
                    size_t count = 0;
                    float weight = 0.0f;       // falloff weight at the INCOMING P
                    Vec3 &P;
                    Vec3 *Cd = nullptr;        // null until ensureCd() is used
                    float *pscale = nullptr;   // null when absent and unneeded
                };

                using PerPointFn = std::function<void(PointCtx &)>;

                // Drives the shared effector loop:
                //   • copies the input geometry,
                //   • loads falloff params at `time`,
                //   • lazily materialises Cd / pscale / orient as requested,
                //   • computes the falloff weight at each point's incoming P,
                //   • invokes the per-effector lambda,
                //   • applies the rotation offset (slerp toward eulerQuat·q0
                //     by weight) when `rotDegFn` reports a non-zero offset,
                //   • applies weight_to_cd debug visualisation last.
                //
                // `rotDegFn` returns the per-point rotation offset in euler
                // degrees (zero → no orient attr created/touched). Pass
                // nullptr for effectors without rotation.
                Geometry applyEffect(std::span<const Geometry *const> inputs,
                                     double time,
                                     bool wantsCd,
                                     bool wantsPscale,
                                     const std::function<Vec3(size_t, float)> &rotDegFn,
                                     const PerPointFn &fn) const
                {
                    setEvalTime(time);
                    if (inputs.empty() || !inputs[0]) return {};
                    Geometry out = *inputs[0];

                    auto *P = out.points().get<Vec3>("P");
                    if (!P) return out;
                    const size_t n = P->data().size();
                    if (n == 0) return out;

                    const FalloffParams falloff = loadFalloffParamsAt(*this, time);

                    auto *Cd = out.points().get<Vec3>("Cd");
                    if ((wantsCd || falloff.weightToCd) && !Cd)
                    {
                        Cd = out.points().add<Vec3>("Cd", Vec3(1.0f));
                    }
                    auto *ps = out.points().get<float>("pscale");
                    if (wantsPscale && !ps)
                    {
                        ps = out.points().add<float>("pscale", 1.0f);
                    }

                    // Rotation: probe whether any point will receive a
                    // non-zero offset before materialising `orient`.
                    auto *orient = out.points().get<Vec4>("orient");
                    const auto *N = out.points().get<Vec3>("N");
                    bool wantsOrient = orient != nullptr;
                    if (!wantsOrient && rotDegFn)
                    {
                        for (size_t i = 0; i < n && !wantsOrient; ++i)
                        {
                            const Vec3 deg = rotDegFn(i, 1.0f);
                            if (deg.x != 0.0f || deg.y != 0.0f || deg.z != 0.0f)
                                wantsOrient = true;
                        }
                    }
                    if (wantsOrient && !orient)
                    {
                        orient = out.points().add<Vec4>("orient", identityWxyz());
                        // Seed from the N frame so effector rotation composes
                        // on top of orient_to_normal-style behaviour.
                        if (N)
                        {
                            for (size_t i = 0; i < n && i < N->data().size(); ++i)
                            {
                                const glm::quat q =
                                    glm::quat_cast(orientFromNormal(N->data()[i]));
                                orient->data()[i] = wxyzFromQuat(q);
                            }
                        }
                    }

                    for (size_t i = 0; i < n; ++i)
                    {
                        const float w = falloffWeight(falloff, P->data()[i]);

                        PointCtx ctx{
                            i, n, w, P->data()[i],
                            (Cd && i < Cd->data().size()) ? &Cd->data()[i] : nullptr,
                            (ps && i < ps->data().size()) ? &ps->data()[i] : nullptr,
                        };
                        fn(ctx);

                        if (orient && rotDegFn && i < orient->data().size())
                        {
                            const Vec3 deg = rotDegFn(i, ctx.weight);
                            if (deg.x != 0.0f || deg.y != 0.0f || deg.z != 0.0f)
                            {
                                const glm::quat q0 = quatFromWxyz(orient->data()[i]);
                                const glm::quat qr =
                                    quatFromWxyz(eulerDegToQuatWxyz(deg));
                                const glm::quat q1 = qr * q0;
                                orient->data()[i] = wxyzFromQuat(
                                    glm::normalize(glm::slerp(q0, q1, ctx.weight)));
                            }
                        }

                        if (falloff.weightToCd && ctx.Cd)
                        {
                            *ctx.Cd = Vec3(w, w, w);
                        }
                    }
                    return out;
                }
            };
        } // namespace mograph
    } // namespace sops
} // namespace tracey
