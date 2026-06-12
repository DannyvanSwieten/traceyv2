// MoGraph "Noise" effector (≈ C4D shader effector with a noise field):
// a 3D perlin field evaluated at each clone's position drives offsets for
// position / rotation / scale / colour, weighted by the shared falloff.
// `offset` is the signature animatable knob — keyframe it (or just its X)
// and the noise flows through the clone array.

#include "../mograph/effector_base.hpp"
#include "../sop_registry.hpp"

#include <glm/gtc/noise.hpp>

#include <algorithm>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class NoiseEffectorSop : public mograph::EffectorSopBase
            {
            public:
                explicit NoiseEffectorSop(size_t uid) : EffectorSopBase(uid)
                {
                    declareParam(Parameter::makeFloat("frequency", 1.0f));
                    declareParam(Parameter::makeVec3("offset", Vec3(0.0f)));
                    declareParam(Parameter::makeInt("seed", 0));
                    declareParam(Parameter::makeVec3("position_amount", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("rotation_amount_deg", Vec3(0.0f)));
                    declareParam(Parameter::makeFloat("scale_amount", 0.0f));
                    declareParam(Parameter::makeBool("use_color", false));
                    declareParam(Parameter::makeFloat("color_amount", 0.5f));
                }

                std::string kind() const override { return "noise_effector"; }

                Geometry cookAt(std::span<const Geometry *const> inputs,
                                double time) const override
                {
                    const float freq = paramFloatAt("frequency", time, 1.0f);
                    const Vec3 offset = paramVec3At("offset", time, Vec3(0.0f));
                    const int seed = paramInt("seed", 0);
                    const Vec3 posAmt = paramVec3At("position_amount", time, Vec3(0.0f));
                    const Vec3 rotAmt = paramVec3At("rotation_amount_deg", time, Vec3(0.0f));
                    const float scaleAmt = paramFloatAt("scale_amount", time, 0.0f);
                    const bool useColor = paramBool("use_color", false);
                    const float colorAmt = paramFloatAt("color_amount", time, 0.5f);

                    const bool wantsScale = scaleAmt != 0.0f;
                    const bool hasRot =
                        rotAmt.x != 0.0f || rotAmt.y != 0.0f || rotAmt.z != 0.0f;

                    // The field is sampled at the INCOMING P; precompute per
                    // point so the rotation lambda (which runs after the
                    // position offset mutated P) sees the same noise vector.
                    std::vector<Vec3> field;
                    if (!inputs.empty() && inputs[0])
                    {
                        const auto &P = inputs[0]->positions();
                        field.reserve(P.size());
                        for (const auto &p : P)
                        {
                            const glm::vec3 base(p.x * freq + offset.x,
                                                 p.y * freq + offset.y,
                                                 p.z * freq + offset.z);
                            field.push_back(Vec3(
                                glm::perlin(mograph::seedShift(base, seed)),
                                glm::perlin(mograph::seedShift(base, seed + 41)),
                                glm::perlin(mograph::seedShift(base, seed + 83))));
                        }
                    }

                    return applyEffect(
                        inputs, time,
                        /*wantsCd=*/useColor,
                        /*wantsPscale=*/wantsScale,
                        [&](size_t i, float) -> Vec3 {
                            if (!hasRot || i >= field.size()) return Vec3(0.0f);
                            const Vec3 &n = field[i];
                            return Vec3(n.x * rotAmt.x, n.y * rotAmt.y, n.z * rotAmt.z);
                        },
                        [&](PointCtx &ctx) {
                            if (ctx.index >= field.size()) return;
                            const float w = ctx.weight;
                            const Vec3 &n = field[ctx.index];
                            ctx.P = ctx.P + Vec3(n.x * posAmt.x, n.y * posAmt.y,
                                                 n.z * posAmt.z) * w;
                            if (ctx.pscale && wantsScale)
                            {
                                const float target =
                                    *ctx.pscale *
                                    std::max(0.0f, 1.0f + n.x * scaleAmt);
                                *ctx.pscale = *ctx.pscale + (target - *ctx.pscale) * w;
                            }
                            if (ctx.Cd && useColor)
                            {
                                const Vec3 c = *ctx.Cd + Vec3(n.x, n.y, n.z) * (colorAmt * w);
                                ctx.Cd->x = std::clamp(c.x, 0.0f, 1.0f);
                                ctx.Cd->y = std::clamp(c.y, 0.0f, 1.0f);
                                ctx.Cd->z = std::clamp(c.z, 0.0f, 1.0f);
                            }
                        });
                }
            };
        }

        void registerNoiseEffectorSop()
        {
            CatalogEntry entry{
                "noise_effector", "Noise Effector", "Effectors",
                /*inputs*/ {{"in"}}, /*outputs*/ {{"out"}},
                /*params*/ {
                    {"frequency", ParamType::Float, "1.0", 0.0, 10.0, 0.05},
                    {"offset", ParamType::Vec3, "[0, 0, 0]"},
                    {"seed", ParamType::Int, "0"},
                    {"position_amount", ParamType::Vec3, "[0, 0, 0]"},
                    {"rotation_amount_deg", ParamType::Vec3, "[0, 0, 0]"},
                    {"scale_amount", ParamType::Float, "0.0", 0.0, 1.0, 0.01},
                    {"use_color", ParamType::Bool, "false"},
                    {"color_amount", ParamType::Float, "0.5", 0.0, 1.0, 0.01},
                }};
            for (auto &spec : mograph::falloffParamSpecs())
                entry.params.push_back(std::move(spec));

            SopRegistry::instance().registerType(
                std::move(entry),
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<NoiseEffectorSop>(uid);
                });
        }
    } // namespace sops
} // namespace tracey
