// pop_attract — pull particles toward a target point. The force is
// `strength * direction` modulated by an inverse-square falloff in the
// `falloff` radius, so particles outside that radius get a weak tug
// and particles near the target get pulled hard (but not infinitely:
// the +1 in the denominator keeps the force finite at distance 0).
// Negative `strength` repels.

#include "../dop_node.hpp"
#include "../dop_graph.hpp"
#include "../dop_registry.hpp"
#include "../sim_state.hpp"

#include "../../core/parallel.hpp"
#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <cmath>
#include <memory>

namespace tracey
{
    namespace dops
    {
        class PopAttractDop : public DopNode
        {
        public:
            explicit PopAttractDop(size_t uid) : DopNode(uid)
            {
                declareParam(Parameter::makeVec3 ("target",   Vec3(0.0f)));
                declareParam(Parameter::makeFloat("strength", 1.0f));
                declareParam(Parameter::makeFloat("falloff",  1.0f));
            }
            std::string kind() const override { return "pop_attract"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                return io;
            }

            void prepare(SimState &state) const override
            {
                Geometry &g = state.geometry;
                if (!g.points().get<Vec3>("force")) g.points().add<Vec3>("force", Vec3(0.0f));
            }

            void cookFrame(DopEvalContext &ctx) const override
            {
                if (!ctx.state) return;
                Geometry &g = ctx.state->geometry;
                auto *P = g.points().get<Vec3>("P");
                auto *F = g.points().get<Vec3>("force");
                if (!P || !F) return;
                const Vec3  tgt  = paramVec3 ("target",   Vec3(0.0f));
                const float k    = paramFloat("strength", 1.0f);
                const float fall = std::max(1e-4f, paramFloat("falloff", 1.0f));
                const float invFall2 = 1.0f / (fall * fall);

                const auto &pd = P->data();
                auto &fd = F->data();
                const size_t n = fd.size();
                tracey::parallel_for_chunks(n,
                    [&pd, &fd, tgt, k, invFall2](size_t begin, size_t end) {
                        for (size_t i = begin; i < end; ++i)
                        {
                            const float dx = tgt.x - pd[i].x;
                            const float dy = tgt.y - pd[i].y;
                            const float dz = tgt.z - pd[i].z;
                            const float r2 = dx*dx + dy*dy + dz*dz;
                            // Inverse-square falloff with a +1 floor so
                            // the force stays finite at distance 0.
                            // weight = 1 / (1 + (r / falloff)^2). At r=0
                            // we get strength*direction; r=falloff gives
                            // half-strength.
                            const float w = k / (1.0f + r2 * invFall2);
                            fd[i].x += dx * w;
                            fd[i].y += dy * w;
                            fd[i].z += dz * w;
                        }
                    });
            }
        };

        void registerPopAttractDop()
        {
            DopRegistry::instance().registerType(
                {"pop_attract", "Attract", "Force",
                 /*inputs*/  {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"target",   ParamType::Vec3,  "[0, 0, 0]"},
                     {"strength", ParamType::Float, "1.0"},
                     {"falloff",  ParamType::Float, "1.0"},
                 }},
                [](size_t uid) { return std::make_unique<PopAttractDop>(uid); });
        }
    }
}
