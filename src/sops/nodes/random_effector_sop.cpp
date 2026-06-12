// MoGraph "Random" effector: deterministic per-clone random offsets for
// position / rotation / scale / colour, weighted by the shared falloff.
// Randomness is keyed by (point index, seed) — stable across cooks, so
// clones don't flicker frame to frame; re-seed for a different arrangement.

#include "../mograph/effector_base.hpp"
#include "../sop_registry.hpp"

#include <algorithm>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class RandomEffectorSop : public mograph::EffectorSopBase
            {
            public:
                explicit RandomEffectorSop(size_t uid) : EffectorSopBase(uid)
                {
                    declareParam(Parameter::makeInt("seed", 0));
                    declareParam(Parameter::makeVec3("position_amount", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("rotation_amount_deg", Vec3(0.0f)));
                    declareParam(Parameter::makeFloat("scale_amount", 0.0f));
                    declareParam(Parameter::makeBool("use_color", false));
                }

                std::string kind() const override { return "random_effector"; }

                Geometry cookAt(std::span<const Geometry *const> inputs,
                                double time) const override
                {
                    const int seed = paramInt("seed", 0);
                    const Vec3 posAmt = paramVec3At("position_amount", time, Vec3(0.0f));
                    const Vec3 rotAmt = paramVec3At("rotation_amount_deg", time, Vec3(0.0f));
                    const float scaleAmt = paramFloatAt("scale_amount", time, 0.0f);
                    const bool useColor = paramBool("use_color", false);

                    const bool wantsScale = scaleAmt != 0.0f;
                    const bool hasRot =
                        rotAmt.x != 0.0f || rotAmt.y != 0.0f || rotAmt.z != 0.0f;

                    return applyEffect(
                        inputs, time,
                        /*wantsCd=*/useColor,
                        /*wantsPscale=*/wantsScale,
                        [&](size_t i, float) -> Vec3 {
                            if (!hasRot) return Vec3(0.0f);
                            const Vec3 r = mograph::rand3Signed(
                                static_cast<uint32_t>(i), seed + 4099);
                            return Vec3(r.x * rotAmt.x, r.y * rotAmt.y, r.z * rotAmt.z);
                        },
                        [&](PointCtx &ctx) {
                            const float w = ctx.weight;
                            const auto idx = static_cast<uint32_t>(ctx.index);
                            const Vec3 r = mograph::rand3Signed(idx, seed);
                            ctx.P = ctx.P + Vec3(r.x * posAmt.x, r.y * posAmt.y,
                                                 r.z * posAmt.z) * w;
                            if (ctx.pscale && wantsScale)
                            {
                                const float target =
                                    *ctx.pscale *
                                    std::max(0.0f, 1.0f + r.x * scaleAmt);
                                *ctx.pscale = *ctx.pscale + (target - *ctx.pscale) * w;
                            }
                            if (ctx.Cd && useColor)
                            {
                                // Second hash stream so colour decorrelates
                                // from the transform randomness.
                                const Vec3 rc = mograph::rand3Signed(idx, seed + 7919);
                                const Vec3 target(rc.x * 0.5f + 0.5f,
                                                  rc.y * 0.5f + 0.5f,
                                                  rc.z * 0.5f + 0.5f);
                                *ctx.Cd = *ctx.Cd + (target - *ctx.Cd) * w;
                            }
                        });
                }
            };
        }

        void registerRandomEffectorSop()
        {
            CatalogEntry entry{
                "random_effector", "Random Effector", "Effectors",
                /*inputs*/ {{"in"}}, /*outputs*/ {{"out"}},
                /*params*/ {
                    {"seed", ParamType::Int, "0"},
                    {"position_amount", ParamType::Vec3, "[0, 0, 0]"},
                    {"rotation_amount_deg", ParamType::Vec3, "[0, 0, 0]"},
                    {"scale_amount", ParamType::Float, "0.0", 0.0, 1.0, 0.01},
                    {"use_color", ParamType::Bool, "false"},
                }};
            for (auto &spec : mograph::falloffParamSpecs())
                entry.params.push_back(std::move(spec));

            SopRegistry::instance().registerType(
                std::move(entry),
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<RandomEffectorSop>(uid);
                });
        }
    } // namespace sops
} // namespace tracey
