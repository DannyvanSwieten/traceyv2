// MoGraph "Plain" effector: a uniform offset (position / rotation / scale /
// colour) applied to every clone, weighted by the shared falloff. The
// C4D workhorse — with a sphere falloff and an animated falloff_center it
// is the classic "wave of displaced clones" rig.

#include "../mograph/effector_base.hpp"
#include "../sop_registry.hpp"

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class PlainEffectorSop : public mograph::EffectorSopBase
            {
            public:
                explicit PlainEffectorSop(size_t uid) : EffectorSopBase(uid)
                {
                    declareParam(Parameter::makeVec3("position", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("rotation_deg", Vec3(0.0f)));
                    declareParam(Parameter::makeFloat("scale", 1.0f));
                    declareParam(Parameter::makeBool("use_color", false));
                    declareParam(Parameter::makeVec3("color", Vec3(1.0f)));
                }

                std::string kind() const override { return "plain_effector"; }

                Geometry cookAt(std::span<const Geometry *const> inputs,
                                double time) const override
                {
                    const Vec3 position = paramVec3At("position", time, Vec3(0.0f));
                    const Vec3 rotDeg = paramVec3At("rotation_deg", time, Vec3(0.0f));
                    const float scale = paramFloatAt("scale", time, 1.0f);
                    const bool useColor = paramBool("use_color", false);
                    const Vec3 color = paramVec3At("color", time, Vec3(1.0f));

                    const bool wantsScale = scale != 1.0f;

                    return applyEffect(
                        inputs, time,
                        /*wantsCd=*/useColor,
                        /*wantsPscale=*/wantsScale,
                        [&](size_t, float) { return rotDeg; },
                        [&](PointCtx &ctx) {
                            const float w = ctx.weight;
                            ctx.P = ctx.P + position * w;
                            if (ctx.pscale && wantsScale)
                            {
                                const float target = *ctx.pscale * scale;
                                *ctx.pscale = *ctx.pscale + (target - *ctx.pscale) * w;
                            }
                            if (ctx.Cd && useColor)
                            {
                                *ctx.Cd = *ctx.Cd + (color - *ctx.Cd) * w;
                            }
                        });
                }
            };
        }

        void registerPlainEffectorSop()
        {
            CatalogEntry entry{
                "plain_effector", "Plain Effector", "Effectors",
                /*inputs*/ {{"in"}}, /*outputs*/ {{"out"}},
                /*params*/ {
                    {"position", ParamType::Vec3, "[0, 0, 0]"},
                    {"rotation_deg", ParamType::Vec3, "[0, 0, 0]"},
                    {"scale", ParamType::Float, "1.0", 0.0, 4.0, 0.01},
                    {"use_color", ParamType::Bool, "false"},
                    {"color", ParamType::Vec3, "[1, 1, 1]"},
                }};
            for (auto &spec : mograph::falloffParamSpecs())
                entry.params.push_back(std::move(spec));

            SopRegistry::instance().registerType(
                std::move(entry),
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<PlainEffectorSop>(uid);
                });
        }
    } // namespace sops
} // namespace tracey
