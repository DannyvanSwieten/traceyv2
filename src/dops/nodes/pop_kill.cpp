// pop_kill — remove particles by predicate. Shares its compaction
// implementation shape with pop_solver (which kills by age >= life)
// and DeleteSop. Three predicate modes:
//   "bbox"       — keep particles inside [min, max] (or outside if
//                   `invert`). The cheapest test, useful for keeping
//                   particles in a sim region.
//   "attribute"  — compare the .x lane of a scalar/vec3 attribute
//                   against a threshold via `op` (>, >=, <, <=, ==,
//                   !=). The point is kept when the predicate FAILS
//                   (so "delete if @Cd.x > 0.9").
//   "age"        — kill when age > max_age. Functionally redundant
//                   with pop_solver's built-in age>=life logic, but
//                   useful when you want a second harder cap or a
//                   per-emitter override.

#include "../dop_node.hpp"
#include "../dop_graph.hpp"
#include "../dop_registry.hpp"
#include "../sim_state.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tracey
{
    namespace dops
    {
        namespace
        {
            template <typename T>
            void compactTyped(AttributeTable &table, const std::string &name,
                              const std::vector<size_t> &kept)
            {
                if (auto *a = table.get<T>(name))
                {
                    auto &d = a->data();
                    for (size_t w = 0; w < kept.size(); ++w)
                    {
                        const size_t r = kept[w];
                        if (w != r) d[w] = d[r];
                    }
                }
            }
            // Mirrors DeleteSop's compareScalar — kept as a small free fn
            // here rather than introducing a shared header for one helper.
            bool compareScalar(float v, const std::string &op, float t)
            {
                if (op == ">")  return v >  t;
                if (op == ">=") return v >= t;
                if (op == "<")  return v <  t;
                if (op == "<=") return v <= t;
                if (op == "==") return v == t;
                if (op == "!=") return v != t;
                return false;
            }
        }

        class PopKillDop : public DopNode
        {
        public:
            explicit PopKillDop(size_t uid) : DopNode(uid)
            {
                declareParam(Parameter::makeString("mode",      "bbox"));
                declareParam(Parameter::makeBool  ("invert",    false));
                declareParam(Parameter::makeVec3  ("bbox_min",  Vec3(-10.0f)));
                declareParam(Parameter::makeVec3  ("bbox_max",  Vec3( 10.0f)));
                declareParam(Parameter::makeString("attr_name", "Cd"));
                declareParam(Parameter::makeString("op",        ">"));
                declareParam(Parameter::makeFloat ("threshold", 0.9f));
                declareParam(Parameter::makeFloat ("max_age",   10.0f));
            }
            std::string kind() const override { return "pop_kill"; }
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
                const size_t n = g.pointCount();
                if (n == 0) return;

                const std::string mode   = paramString("mode", "bbox");
                const bool        invert = paramBool("invert", false);

                std::vector<uint8_t> keep(n, 1);
                if (mode == "bbox")
                {
                    const Vec3 lo = paramVec3("bbox_min", Vec3(-10.0f));
                    const Vec3 hi = paramVec3("bbox_max", Vec3( 10.0f));
                    const auto &P = g.positions();
                    for (size_t i = 0; i < n; ++i)
                    {
                        const Vec3 &p = P[i];
                        const bool inside =
                            p.x >= lo.x && p.x <= hi.x &&
                            p.y >= lo.y && p.y <= hi.y &&
                            p.z >= lo.z && p.z <= hi.z;
                        keep[i] = inside ? 1 : 0;
                    }
                }
                else if (mode == "attribute")
                {
                    const std::string name = paramString("attr_name", "Cd");
                    const std::string op   = paramString("op", ">");
                    const float       t    = paramFloat("threshold", 0.9f);
                    if (const auto *a = g.points().get<float>(name))
                    {
                        const auto &d = a->data();
                        // "kill if predicate" → "keep if NOT predicate".
                        for (size_t i = 0; i < n; ++i)
                            keep[i] = compareScalar(d[i], op, t) ? 0 : 1;
                    }
                    else if (const auto *a = g.points().get<Vec3>(name))
                    {
                        const auto &d = a->data();
                        for (size_t i = 0; i < n; ++i)
                            keep[i] = compareScalar(d[i].x, op, t) ? 0 : 1;
                    }
                }
                else if (mode == "age")
                {
                    const float maxAge = paramFloat("max_age", 10.0f);
                    if (const auto *A = g.points().get<float>("age"))
                    {
                        const auto &d = A->data();
                        for (size_t i = 0; i < n; ++i)
                            keep[i] = (d[i] > maxAge) ? 0 : 1;
                    }
                }

                if (invert)
                    for (auto &k : keep) k = k ? 0 : 1;

                std::vector<size_t> kept;
                kept.reserve(n);
                for (size_t i = 0; i < n; ++i)
                    if (keep[i]) kept.push_back(i);
                if (kept.size() == n) return;

                // Particles have no primitives (pop_source emits a
                // point-only Geometry), so an in-place point compaction
                // is enough. Mirrors pop_solver's compaction shape.
                const auto names = g.points().names();
                for (const auto &name : names)
                {
                    compactTyped<float>(g.points(), name, kept);
                    compactTyped<int>  (g.points(), name, kept);
                    compactTyped<Vec2> (g.points(), name, kept);
                    compactTyped<Vec3> (g.points(), name, kept);
                    compactTyped<Vec4> (g.points(), name, kept);
                }
                g.points().resize(kept.size());
            }
        };

        void registerPopKillDop()
        {
            DopRegistry::instance().registerType(
                {"pop_kill", "Kill", "Modifier",
                 /*inputs*/  {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"mode",      ParamType::String, "\"bbox\""},
                     {"invert",    ParamType::Bool,   "false"},
                     {"bbox_min",  ParamType::Vec3,   "[-10, -10, -10]"},
                     {"bbox_max",  ParamType::Vec3,   "[10, 10, 10]"},
                     {"attr_name", ParamType::String, "\"Cd\""},
                     {"op",        ParamType::String, "\">\""},
                     {"threshold", ParamType::Float,  "0.9"},
                     {"max_age",   ParamType::Float,  "10.0"},
                 }},
                [](size_t uid) { return std::make_unique<PopKillDop>(uid); });
        }
    }
}
