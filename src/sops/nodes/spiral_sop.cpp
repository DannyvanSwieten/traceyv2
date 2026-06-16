// Generator SOP: a helix / conical spiral of ordered points around +Y.
// `turns` controls revolutions, `height` the rise, and `end_radius` lets the
// radius taper from `radius` (bottom) to `end_radius` (top) for cone spirals.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"
#include "../mograph/curve_frame.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class SpiralSop : public SopNode
            {
            public:
                explicit SpiralSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("radius", 1.0f));
                    declareParam(Parameter::makeFloat("end_radius", 1.0f));
                    declareParam(Parameter::makeFloat("height", 2.0f));
                    declareParam(Parameter::makeFloat("turns", 3.0f));
                    declareParam(Parameter::makeInt("points", 64));
                }

                std::string kind() const override { return "spiral"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    const float r0 = paramFloat("radius", 1.0f);
                    const float r1 = paramFloat("end_radius", 1.0f);
                    const float height = paramFloat("height", 2.0f);
                    const float turns = paramFloat("turns", 3.0f);
                    const int count = std::max(2, paramInt("points", 64));

                    const float twoPi = 2.0f * pi<float>();

                    Geometry g;
                    auto &pts = g.points();
                    auto *P = pts.add<Vec3>("P", Vec3(0.0f));
                    auto *ps = pts.add<float>("pscale", 1.0f);
                    g.resizePoints(static_cast<size_t>(count));

                    auto &Pd = P->data();
                    auto &psd = ps->data();
                    for (int i = 0; i < count; ++i)
                    {
                        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
                        const float a = twoPi * turns * t;
                        const float r = r0 + (r1 - r0) * t;
                        Pd[i] = Vec3(r * std::cos(a), height * t, r * std::sin(a));
                        psd[i] = 1.0f;
                    }

                    mograph::setCurveClosed(g, false);
                    mograph::writeCurveFrames(g, false);
                    return g;
                }
            };
        }

        void registerSpiralSop()
        {
            SopRegistry::instance().registerType(
                {"spiral", "Spiral", "Generators",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"radius", ParamType::Float, "1.0", 0.0, 20.0, 0.01},
                     {"end_radius", ParamType::Float, "1.0", 0.0, 20.0, 0.01},
                     {"height", ParamType::Float, "2.0", 0.0, 50.0, 0.01},
                     {"turns", ParamType::Float, "3.0", 0.0, 64.0, 0.1},
                     {"points", ParamType::Int, "64", 2.0, 4000.0, 1.0},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<SpiralSop>(uid);
                });
        }
    }
}
