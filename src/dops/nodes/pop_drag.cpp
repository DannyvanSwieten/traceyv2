// pop_drag — viscous drag. F += -k * v for every particle. With mass=1
// the unit on `drag` is 1/seconds; a value of 1.0 halves velocity in
// about ln(2) seconds, 5.0 settles a particle in roughly half a second.
//
// Sits between the other force nodes and pop_solver so its contribution
// hits the same `force` accumulator the solver consumes.

#include "../dop_node.hpp"
#include "../dop_graph.hpp"
#include "../dop_registry.hpp"
#include "../sim_state.hpp"

#include "../../core/parallel.hpp"
#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <memory>

namespace tracey
{
    namespace dops
    {
        class PopDragDop : public DopNode
        {
        public:
            explicit PopDragDop(size_t uid) : DopNode(uid)
            {
                declareParam(Parameter::makeFloat("drag", 1.0f));
            }
            std::string kind() const override { return "pop_drag"; }
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
                if (!g.points().get<Vec3>("v"))     g.points().add<Vec3>("v",     Vec3(0.0f));
            }

            void cookFrame(DopEvalContext &ctx) const override
            {
                if (!ctx.state) return;
                Geometry &g = ctx.state->geometry;
                auto *F = g.points().get<Vec3>("force");
                auto *V = g.points().get<Vec3>("v");
                if (!F || !V) return;
                const float k = paramFloat("drag", 1.0f);
                auto &fd = F->data();
                const auto &vd = V->data();
                const size_t n = fd.size();
                tracey::parallel_for_chunks(n,
                    [&fd, &vd, k](size_t begin, size_t end) {
                        for (size_t i = begin; i < end; ++i)
                        {
                            fd[i].x -= k * vd[i].x;
                            fd[i].y -= k * vd[i].y;
                            fd[i].z -= k * vd[i].z;
                        }
                    });
            }
        };

        void registerPopDragDop()
        {
            DopRegistry::instance().registerType(
                {"pop_drag", "Drag", "Force",
                 /*inputs*/  {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"drag", ParamType::Float, "1.0"},
                 }},
                [](size_t uid) { return std::make_unique<PopDragDop>(uid); });
        }
    }
}
