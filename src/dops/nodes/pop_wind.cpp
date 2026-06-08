// pop_wind — uniform wind plus optional Perlin turbulence. The uniform
// component is `direction * speed` (so toggling sign on `speed` flips
// the wind without touching `direction`); the turbulence component
// adds `perlin(P * freq) * turbulence` per axis, decorrelated across
// x/y/z via the same seed-shift trick noise_curl uses. With both
// `speed` and `turbulence` left at 0 the node is a no-op.

#include "../dop_node.hpp"
#include "../dop_graph.hpp"
#include "../dop_registry.hpp"
#include "../sim_state.hpp"

#include "../../core/parallel.hpp"
#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/noise.hpp>

#include <memory>

namespace tracey
{
    namespace dops
    {
        namespace
        {
            // Decorrelated 3D perlin: shift the lookup coord per axis so
            // the three components don't all spike together. Matches the
            // shift constants noise_vec3 uses in glsl_emit so a graph that
            // mixes CPU pop_wind with GPU noise_vec3 reads consistently.
            inline glm::vec3 perlin3(const glm::vec3 &p, int seed)
            {
                const float s = static_cast<float>(seed);
                return glm::vec3(
                    glm::perlin(glm::vec3(p.x + s * 17.13f, p.y + s * 31.71f, p.z + s * 53.91f)),
                    glm::perlin(glm::vec3(p.x + (s + 41.f) * 17.13f, p.y + (s + 41.f) * 31.71f, p.z + (s + 41.f) * 53.91f)),
                    glm::perlin(glm::vec3(p.x + (s + 83.f) * 17.13f, p.y + (s + 83.f) * 31.71f, p.z + (s + 83.f) * 53.91f))
                );
            }
        }

        class PopWindDop : public DopNode
        {
        public:
            explicit PopWindDop(size_t uid) : DopNode(uid)
            {
                declareParam(Parameter::makeVec3 ("direction",       Vec3(1.0f, 0.0f, 0.0f)));
                declareParam(Parameter::makeFloat("speed",            1.0f));
                declareParam(Parameter::makeFloat("turbulence",       0.0f));
                declareParam(Parameter::makeFloat("turbulence_freq",  1.0f));
                declareParam(Parameter::makeInt  ("seed",             0));
            }
            std::string kind() const override { return "pop_wind"; }
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

                const Vec3 dir = paramVec3("direction", Vec3(1.0f, 0.0f, 0.0f));
                const float speed   = paramFloat("speed", 1.0f);
                const float turb    = paramFloat("turbulence", 0.0f);
                const float freq    = paramFloat("turbulence_freq", 1.0f);
                const int   seed    = paramInt("seed", 0);

                const Vec3 uniform(dir.x * speed, dir.y * speed, dir.z * speed);
                const bool wantTurb = std::abs(turb) > 1e-6f && std::abs(freq) > 1e-6f;

                const auto &pd = P->data();
                auto &fd = F->data();
                const size_t n = fd.size();
                tracey::parallel_for_chunks(n,
                    [&pd, &fd, uniform, turb, freq, seed, wantTurb](size_t begin, size_t end) {
                        for (size_t i = begin; i < end; ++i)
                        {
                            fd[i].x += uniform.x;
                            fd[i].y += uniform.y;
                            fd[i].z += uniform.z;
                            if (wantTurb)
                            {
                                const glm::vec3 sp(pd[i].x * freq, pd[i].y * freq, pd[i].z * freq);
                                const glm::vec3 t = perlin3(sp, seed) * turb;
                                fd[i].x += t.x;
                                fd[i].y += t.y;
                                fd[i].z += t.z;
                            }
                        }
                    });
            }
        };

        void registerPopWindDop()
        {
            DopRegistry::instance().registerType(
                {"pop_wind", "Wind", "Force",
                 /*inputs*/  {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"direction",       ParamType::Vec3,  "[1, 0, 0]"},
                     {"speed",           ParamType::Float, "1.0"},
                     {"turbulence",      ParamType::Float, "0.0"},
                     {"turbulence_freq", ParamType::Float, "1.0"},
                     {"seed",            ParamType::Int,   "0"},
                 }},
                [](size_t uid) { return std::make_unique<PopWindDop>(uid); });
        }
    }
}
