// Generator SOP: a circle / arc of ordered points in the XZ (ground) plane,
// so clones placed along it stand upright by default. `arc` < 360 produces an
// open arc; a full 360 is a closed curve (the cloner/sweep wrap the seam).

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
            class CircleSop : public SopNode
            {
            public:
                explicit CircleSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("radius", 1.0f));
                    declareParam(Parameter::makeInt("segments", 32));
                    declareParam(Parameter::makeFloat("arc", 360.0f));
                }

                std::string kind() const override { return "circle"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    const float radius = paramFloat("radius", 1.0f);
                    const int segments = std::max(3, paramInt("segments", 32));
                    const float arcDeg = clamp(paramFloat("arc", 360.0f), 0.0f, 360.0f);
                    const bool closed = arcDeg >= 359.999f;

                    // For a closed ring of N segments we want N points (the
                    // wrap edge N-1 -> 0 is implicit). For an open arc we want
                    // segments+1 points so both endpoints land on the arc.
                    const int count = closed ? segments : segments + 1;
                    const float arcRad = radians(arcDeg);
                    const float denom = closed ? static_cast<float>(segments)
                                               : static_cast<float>(segments);

                    Geometry g;
                    auto &pts = g.points();
                    auto *P = pts.add<Vec3>("P", Vec3(0.0f));
                    auto *ps = pts.add<float>("pscale", 1.0f);
                    g.resizePoints(static_cast<size_t>(count));

                    auto &Pd = P->data();
                    auto &psd = ps->data();
                    for (int i = 0; i < count; ++i)
                    {
                        const float a = arcRad * (static_cast<float>(i) / denom);
                        Pd[i] = Vec3(radius * std::cos(a), 0.0f, radius * std::sin(a));
                        psd[i] = 1.0f;
                    }

                    mograph::setCurveClosed(g, closed);
                    mograph::writeCurveFrames(g, closed);
                    return g;
                }
            };
        }

        void registerCircleSop()
        {
            SopRegistry::instance().registerType(
                {"circle", "Circle", "Generators",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"radius", ParamType::Float, "1.0", 0.05, 20.0, 0.01},
                     {"segments", ParamType::Int, "32", 3.0, 512.0, 1.0},
                     {"arc", ParamType::Float, "360.0", 0.0, 360.0, 1.0},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<CircleSop>(uid);
                });
        }
    }
}
