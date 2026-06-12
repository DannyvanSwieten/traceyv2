// MoGraph "Step" effector: ramps the effect across the clone INDEX —
// t = i/(count-1), shaped by a power curve, optionally reversed — and
// multiplies that ramp ON TOP of the spatial falloff. The classic
// staircase / cascade rig (scale steps, rotation fans, colour gradients).

#include "../mograph/effector_base.hpp"
#include "../sop_registry.hpp"

#include <cmath>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class StepEffectorSop : public mograph::EffectorSopBase
            {
            public:
                explicit StepEffectorSop(size_t uid) : EffectorSopBase(uid)
                {
                    declareParam(Parameter::makeFloat("shaping", 1.0f));
                    declareParam(Parameter::makeBool("reverse", false));
                    declareParam(Parameter::makeVec3("position", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("rotation_deg", Vec3(0.0f)));
                    declareParam(Parameter::makeFloat("scale", 1.0f));
                }

                std::string kind() const override { return "step_effector"; }

                Geometry cookAt(std::span<const Geometry *const> inputs,
                                double time) const override
                {
                    const float shaping =
                        std::max(0.01f, paramFloatAt("shaping", time, 1.0f));
                    const bool reverse = paramBool("reverse", false);
                    const Vec3 position = paramVec3At("position", time, Vec3(0.0f));
                    const Vec3 rotDeg = paramVec3At("rotation_deg", time, Vec3(0.0f));
                    const float scale = paramFloatAt("scale", time, 1.0f);

                    const bool wantsScale = scale != 1.0f;

                    auto ramp = [&](size_t i, size_t count) -> float {
                        float t = count > 1
                                      ? static_cast<float>(i) /
                                            static_cast<float>(count - 1)
                                      : 1.0f;
                        if (reverse) t = 1.0f - t;
                        return std::pow(t, shaping);
                    };

                    // Rotation lambda gets the count via capture-by-need: the
                    // base supplies only (index, weight), so we read the input
                    // point count up front.
                    const size_t count =
                        (!inputs.empty() && inputs[0]) ? inputs[0]->pointCount() : 0;

                    return applyEffect(
                        inputs, time,
                        /*wantsCd=*/false,
                        /*wantsPscale=*/wantsScale,
                        [&](size_t i, float) -> Vec3 {
                            const float t = ramp(i, count);
                            return Vec3(rotDeg.x * t, rotDeg.y * t, rotDeg.z * t);
                        },
                        [&](PointCtx &ctx) {
                            const float w = ctx.weight * ramp(ctx.index, ctx.count);
                            ctx.P = ctx.P + position * w;
                            if (ctx.pscale && wantsScale)
                            {
                                const float target = *ctx.pscale * scale;
                                *ctx.pscale = *ctx.pscale + (target - *ctx.pscale) * w;
                            }
                        });
                }
            };
        }

        void registerStepEffectorSop()
        {
            CatalogEntry entry{
                "step_effector", "Step Effector", "Effectors",
                /*inputs*/ {{"in"}}, /*outputs*/ {{"out"}},
                /*params*/ {
                    {"shaping", ParamType::Float, "1.0", 0.01, 8.0, 0.01},
                    {"reverse", ParamType::Bool, "false"},
                    {"position", ParamType::Vec3, "[0, 0, 0]"},
                    {"rotation_deg", ParamType::Vec3, "[0, 0, 0]"},
                    {"scale", ParamType::Float, "1.0", 0.0, 4.0, 0.01},
                }};
            for (auto &spec : mograph::falloffParamSpecs())
                entry.params.push_back(std::move(spec));

            SopRegistry::instance().registerType(
                std::move(entry),
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<StepEffectorSop>(uid);
                });
        }
    } // namespace sops
} // namespace tracey
