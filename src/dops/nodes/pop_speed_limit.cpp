// pop_speed_limit — clamp |v| to `max_speed` (or rescale toward
// `target_speed` smoothly if `mode` is "soft"). Place AFTER pop_solver
// in the chain — the solver has already integrated v at that point,
// so this clamp catches any runaway resulting from feedback loops
// (attract-into-target, drag-too-low, large dt with strong forces).
//
// Modes:
//   "hard" (default) — if |v| > max_speed: v = v / |v| * max_speed.
//                       Below max_speed: untouched.
//   "soft"           — exponential damp toward max_speed when over,
//                       so the velocity decays toward the cap over a
//                       few frames instead of slamming. Uses dt from
//                       the sim header.

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
#include <string>

namespace tracey
{
    namespace dops
    {
        class PopSpeedLimitDop : public DopNode
        {
        public:
            explicit PopSpeedLimitDop(size_t uid) : DopNode(uid)
            {
                declareParam(Parameter::makeString("mode",      "hard"));
                declareParam(Parameter::makeFloat ("max_speed", 10.0f));
                // Higher = snappier convergence toward the cap in soft mode.
                declareParam(Parameter::makeFloat ("soft_rate", 4.0f));
            }
            std::string kind() const override { return "pop_speed_limit"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                return io;
            }

            void cookFrame(DopEvalContext &ctx) const override
            {
                if (!ctx.state) return;
                Geometry &g = ctx.state->geometry;
                auto *V = g.points().get<Vec3>("v");
                if (!V) return;
                const std::string mode = paramString("mode", "hard");
                const float       cap  = std::max(0.0f, paramFloat("max_speed", 10.0f));
                const float       rate = std::max(0.0f, paramFloat("soft_rate", 4.0f));
                const float       dt   = std::max(0.0f, static_cast<float>(ctx.state->header.dt));
                const bool soft = (mode == "soft");

                auto &vd = V->data();
                const size_t n = vd.size();
                tracey::parallel_for_chunks(n,
                    [&vd, cap, rate, dt, soft](size_t begin, size_t end) {
                        for (size_t i = begin; i < end; ++i)
                        {
                            const float speed2 =
                                vd[i].x*vd[i].x + vd[i].y*vd[i].y + vd[i].z*vd[i].z;
                            if (speed2 <= cap * cap) continue;
                            const float speed = std::sqrt(speed2);
                            float target = cap;
                            if (soft)
                            {
                                // Exponential decay toward cap. Standard
                                // implicit-Euler-like step so the result
                                // is dt-stable: speed_new = speed + (cap - speed)
                                // * (1 - exp(-rate*dt)).
                                const float a = 1.0f - std::exp(-rate * dt);
                                target = speed + (cap - speed) * a;
                            }
                            const float s = target / speed;
                            vd[i].x *= s;
                            vd[i].y *= s;
                            vd[i].z *= s;
                        }
                    });
            }
        };

        void registerPopSpeedLimitDop()
        {
            DopRegistry::instance().registerType(
                {"pop_speed_limit", "Speed Limit", "Modifier",
                 /*inputs*/  {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"mode",      ParamType::String, "\"hard\""},
                     {"max_speed", ParamType::Float,  "10.0"},
                     {"soft_rate", ParamType::Float,  "4.0"},
                 }},
                [](size_t uid) { return std::make_unique<PopSpeedLimitDop>(uid); });
        }
    }
}
