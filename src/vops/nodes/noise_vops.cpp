#include "../register_builtins.hpp"
#include "../vop_node.hpp"
#include "../vop_graph.hpp"
#include "../vop_registry.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/noise.hpp>

#include <memory>

namespace tracey
{
    namespace vops
    {
        // ── noise_perlin ─────────────────────────────────────────────────────
        // Sample a 3D Perlin noise field at the input position scaled by
        // `frequency` and offset by a per-instance seed (so users can vary
        // noise patterns without moving sample points). Output is scaled by
        // `amplitude`. Returns a Float in roughly [-amplitude, +amplitude].
        class NoisePerlinVop : public VopNode
        {
        public:
            explicit NoisePerlinVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("frequency", 1.0f));
                declareParam(Parameter::makeFloat("amplitude", 1.0f));
                declareParam(Parameter::makeInt("seed", 0));
            }
            std::string kind() const override { return "noise_perlin"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("p", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                auto in = ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)});
                Vec3 p(0.0f);
                if (auto *v = std::get_if<Vec3>(&in)) p = *v;
                else if (auto *f = std::get_if<float>(&in)) p = Vec3(*f);

                const float freq = paramFloat("frequency", 1.0f);
                const float amp  = paramFloat("amplitude", 1.0f);
                const int   seed = paramInt("seed", 0);

                // Seed shifts the sample point in glm::perlin's continuous
                // domain. The offsets are arbitrary primes-ish multipliers so
                // adjacent integer seeds give meaningfully different fields.
                const float so = static_cast<float>(seed);
                glm::vec3 sp(p.x * freq + so * 17.13f,
                             p.y * freq + so * 31.71f,
                             p.z * freq + so * 53.91f);
                const float n = glm::perlin(sp);
                ctx.graph->writeOutput(ctx, uid(), 0, n * amp);
            }
        };

        namespace
        {
            template <typename T>
            VopRegistry::Factory makeFactory()
            {
                return [](size_t uid) { return std::make_unique<T>(uid); };
            }
        }

        void registerNoiseVops()
        {
            auto &reg = VopRegistry::instance();
            reg.registerType(
                {"noise_perlin", "Perlin Noise", "Noise",
                 /*inputs*/ {{"p"}}, /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"frequency", ParamType::Float, "1.0"},
                     {"amplitude", ParamType::Float, "1.0"},
                     {"seed",      ParamType::Int,   "0"},
                 }},
                makeFactory<NoisePerlinVop>());
        }
    }
}
