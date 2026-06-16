// Generator SOP: a straight line of ordered points from `start` to `end`.
// A "curve" here is an ordered point cloud (point index = path order) plus a
// Detail `closed` flag and per-point N + orient frames (see curve_frame.hpp).
// Feed it to Resample, Sweep, or directly to a cloner (clone-along-line).

#include "../sop_node.hpp"
#include "../sop_registry.hpp"
#include "../mograph/curve_frame.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <memory>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class LineSop : public SopNode
            {
            public:
                explicit LineSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeVec3("start", Vec3(-1.0f, 0.0f, 0.0f)));
                    declareParam(Parameter::makeVec3("end", Vec3(1.0f, 0.0f, 0.0f)));
                    declareParam(Parameter::makeInt("points", 10));
                }

                std::string kind() const override { return "line"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    const Vec3 start = paramVec3("start", Vec3(-1.0f, 0.0f, 0.0f));
                    const Vec3 end = paramVec3("end", Vec3(1.0f, 0.0f, 0.0f));
                    const int count = std::max(2, paramInt("points", 10));

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
                        Pd[i] = start + (end - start) * t;
                        psd[i] = 1.0f;
                    }

                    mograph::setCurveClosed(g, false);
                    mograph::writeCurveFrames(g, false);
                    return g;
                }
            };
        }

        void registerLineSop()
        {
            SopRegistry::instance().registerType(
                {"line", "Line", "Generators",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"start", ParamType::Vec3, "[-1, 0, 0]"},
                     {"end", ParamType::Vec3, "[1, 0, 0]"},
                     {"points", ParamType::Int, "10", 2.0, 1000.0, 1.0},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<LineSop>(uid);
                });
        }
    }
}
