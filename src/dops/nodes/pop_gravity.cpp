// pop_gravity — adds a constant gravity vector to the `force` point
// attribute every substep. With mass=1 (the v1 default), F = a, so the
// vector is the acceleration directly. Pair with pop_solver downstream
// to actually integrate.

#include "../dop_node.hpp"
#include "../dop_graph.hpp"
#include "../dop_registry.hpp"
#include "../sim_state.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <memory>

namespace tracey
{
    namespace dops
    {
        class PopGravityDop : public DopNode
        {
        public:
            explicit PopGravityDop(size_t uid) : DopNode(uid)
            {
                // Earth-ish default. Negative Y is "down" in tracey's
                // right-handed Y-up convention.
                declareParam(Parameter::makeVec3("gravity", Vec3(0.0f, -9.81f, 0.0f)));
            }
            std::string kind() const override { return "pop_gravity"; }
            InputsAndOutputs ports() const override { return InputsAndOutputs{}; }

            void prepare(SimState &state) const override
            {
                // `force` must exist before we accumulate into it. pop_source
                // adds it on emit, but a graph with pop_gravity but no
                // pop_source (or pop_gravity ordered before pop_source) would
                // crash without this.
                if (!state.geometry.points().get<Vec3>("force"))
                    state.geometry.points().add<Vec3>("force", Vec3(0.0f));
            }

            void cookFrame(DopEvalContext &ctx) const override
            {
                if (!ctx.state) return;
                Geometry &g = ctx.state->geometry;
                auto *F = g.points().get<Vec3>("force");
                if (!F) return;
                const Vec3 gv = paramVec3("gravity", Vec3(0.0f, -9.81f, 0.0f));
                auto &fd = F->data();
                for (auto &f : fd)
                {
                    f.x += gv.x;
                    f.y += gv.y;
                    f.z += gv.z;
                }
            }
        };

        namespace
        {
            template <typename T>
            DopRegistry::Factory makeFactory()
            {
                return [](size_t uid) { return std::make_unique<T>(uid); };
            }
        }

        void registerForceDops()
        {
            auto &reg = DopRegistry::instance();
            reg.registerType(
                {"pop_gravity", "Gravity", "Force",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"gravity", ParamType::Vec3, "[0, -9.81, 0]"},
                 }},
                makeFactory<PopGravityDop>());
        }
    }
}
