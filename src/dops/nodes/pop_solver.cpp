// pop_solver — the particle integrator.
//
// Phase 1: forward Euler. v += force * dt; P += v * dt; age += dt. After
// integrating, particles whose age >= life are removed via in-place
// compaction of every point attribute. The `force` accumulator is zeroed
// at the end of the step so the next substep starts fresh and forces
// don't double-apply across the boundary.
//
// pop_solver should be the terminal node in any particle DOP graph —
// sources (pop_source) emit, forces (pop_gravity, pop_force) accumulate
// into `force`, the solver integrates + compacts.

#include "../dop_node.hpp"
#include "../dop_graph.hpp"
#include "../dop_registry.hpp"
#include "../sim_state.hpp"

#include "../../core/parallel.hpp"
#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tracey
{
    namespace dops
    {
        namespace
        {
            // In-place compaction. For each typed point attribute, keep
            // only the entries at `keptIdx`, then shrink the table to
            // keptIdx.size(). Particles have no triangle topology so we
            // don't have to touch vertex / primitive tables.
            template <typename T>
            void compactTyped(AttributeTable &table,
                              const std::string &name,
                              const std::vector<size_t> &keptIdx)
            {
                if (auto *a = table.get<T>(name))
                {
                    auto &d = a->data();
                    for (size_t w = 0; w < keptIdx.size(); ++w)
                    {
                        const size_t r = keptIdx[w];
                        if (w != r) d[w] = d[r];
                    }
                }
            }
        }

        class PopSolverDop : public DopNode
        {
        public:
            explicit PopSolverDop(size_t uid) : DopNode(uid) {}
            std::string kind() const override { return "pop_solver"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                return io;
            }

            void prepare(SimState &state) const override
            {
                // Make sure the attributes we read/write exist even when
                // pop_source isn't present (e.g. a manually-seeded state
                // from an input SOP in some future setup).
                Geometry &g = state.geometry;
                if (!g.points().get<Vec3> ("v"))     g.points().add<Vec3> ("v",     Vec3(0.0f));
                if (!g.points().get<float>("age"))   g.points().add<float>("age",   0.0f);
                if (!g.points().get<float>("life"))  g.points().add<float>("life",  1.0f);
                if (!g.points().get<Vec3> ("force")) g.points().add<Vec3> ("force", Vec3(0.0f));
            }

            void cookFrame(DopEvalContext &ctx) const override
            {
                if (!ctx.state) return;
                Geometry &g = ctx.state->geometry;
                const float dt = static_cast<float>(ctx.state->header.dt);
                if (dt <= 0.0f) return;

                auto *P     = g.points().get<Vec3> ("P");
                auto *V     = g.points().get<Vec3> ("v");
                auto *AGE   = g.points().get<float>("age");
                auto *LIFE  = g.points().get<float>("life");
                auto *FORCE = g.points().get<Vec3> ("force");
                if (!P || !V || !AGE || !LIFE || !FORCE) return;

                auto &pd = P->data();
                auto &vd = V->data();
                auto &ad = AGE->data();
                auto &ld = LIFE->data();
                auto &fd = FORCE->data();
                const size_t n = pd.size();

                // Forward Euler, parallel over particles. With mass=1
                // the acceleration is just the accumulated force; each
                // iteration touches only its own index across all five
                // arrays so there's no aliasing across worker threads.
                // Zero the force accumulator after consuming so the
                // next substep's force nodes start from 0 again.
                tracey::parallel_for_chunks(n,
                    [&pd, &vd, &ad, &fd, dt](size_t begin, size_t end) {
                        for (size_t i = begin; i < end; ++i)
                        {
                            vd[i].x += fd[i].x * dt;
                            vd[i].y += fd[i].y * dt;
                            vd[i].z += fd[i].z * dt;
                            pd[i].x += vd[i].x * dt;
                            pd[i].y += vd[i].y * dt;
                            pd[i].z += vd[i].z * dt;
                            ad[i]   += dt;
                            fd[i] = Vec3(0.0f);
                        }
                    });

                // Build the kept-index list. Particles with age >= life
                // get dropped this substep.
                std::vector<size_t> kept;
                kept.reserve(n);
                for (size_t i = 0; i < n; ++i)
                {
                    if (ad[i] < ld[i]) kept.push_back(i);
                }
                if (kept.size() == n) return; // no kills

                // Compact every typed point attribute. Phase 1 covers the
                // particle-essential types; widen here if you add new
                // typed point attributes.
                const auto names = g.points().names();
                for (const auto &name : names)
                {
                    compactTyped<float>      (g.points(), name, kept);
                    compactTyped<int>        (g.points(), name, kept);
                    compactTyped<Vec2>       (g.points(), name, kept);
                    compactTyped<Vec3>       (g.points(), name, kept);
                    compactTyped<Vec4>       (g.points(), name, kept);
                }
                g.points().resize(kept.size());
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

        void registerSolverDops()
        {
            auto &reg = DopRegistry::instance();
            reg.registerType(
                {"pop_solver", "Particle Solver", "Solver",
                 /*inputs*/ {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {}},
                makeFactory<PopSolverDop>());
        }
    }
}
