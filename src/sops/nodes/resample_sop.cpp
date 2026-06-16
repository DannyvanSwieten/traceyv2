// Modifier SOP: resample a curve (ordered point cloud) to evenly-spaced
// points by arc length — either a fixed `count` or a target `segment_length`.
// Recomputes the per-point frames (N + orient) so clone spacing along a path
// stays uniform. This is the spacing control for clone-along-spline and the
// step that conditions a curve before Sweep.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"
#include "../mograph/curve_frame.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class ResampleSop : public SopNode
            {
            public:
                explicit ResampleSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("mode", "count"));
                    declareParam(Parameter::makeInt("count", 50));
                    declareParam(Parameter::makeFloat("segment_length", 0.5f));
                    declareParam(Parameter::makeBool("recompute_frames", true));
                }

                std::string kind() const override { return "resample"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    if (inputs.empty() || !inputs[0]) return {};
                    const Geometry &in = *inputs[0];
                    const auto &P = in.positions();
                    const size_t n = P.size();
                    if (n < 2) return in; // nothing to resample

                    const bool closed = mograph::curveClosed(in);
                    const std::string mode = paramString("mode", "count");
                    const bool recomputeFrames = paramBool("recompute_frames", true);

                    // ── cumulative arc length (cum[k] = length up to point k);
                    //    for closed curves include the wrap edge as the last span ──
                    const size_t spans = closed ? n : n - 1;
                    std::vector<float> cum(spans + 1, 0.0f);
                    for (size_t i = 0; i < spans; ++i)
                        cum[i + 1] = cum[i] + glm::length(P[(i + 1) % n] - P[i]);
                    const float total = cum[spans];
                    if (total < 1e-8f) return in; // degenerate (coincident points)

                    // ── target sample count ──
                    int count;
                    if (mode == "length")
                    {
                        const float seg = std::max(1e-4f, paramFloat("segment_length", 0.5f));
                        const int divs = std::max(1, static_cast<int>(std::lround(total / seg)));
                        count = closed ? std::max(3, divs) : divs + 1;
                    }
                    else
                    {
                        count = std::max(closed ? 3 : 2, paramInt("count", 50));
                    }

                    // Optional source attributes interpolated alongside P.
                    const auto *inPs = in.points().get<float>("pscale");
                    const auto *inCd = in.points().get<Vec3>("Cd");

                    Geometry out;
                    auto &pts = out.points();
                    auto *oP = pts.add<Vec3>("P", Vec3(0.0f));
                    auto *oPs = pts.add<float>("pscale", 1.0f);
                    Attribute<Vec3> *oCd = inCd ? pts.add<Vec3>("Cd", Vec3(1.0f)) : nullptr;
                    out.resizePoints(static_cast<size_t>(count));

                    auto &oPd = oP->data();
                    auto &oPsd = oPs->data();

                    // Sample at arc length s: locate the span and lerp endpoints.
                    auto sampleAt = [&](float s, size_t &outIdx) {
                        // advance through spans; clamp to the last span at the end
                        size_t seg = 0;
                        while (seg + 1 < spans && cum[seg + 1] < s) ++seg;
                        const float segLen = cum[seg + 1] - cum[seg];
                        const float t = segLen > 1e-8f ? (s - cum[seg]) / segLen : 0.0f;
                        const size_t a = seg;
                        const size_t b = (seg + 1) % n;
                        oPd[outIdx] = P[a] + (P[b] - P[a]) * t;
                        oPsd[outIdx] = inPs ? (inPs->data()[a] + (inPs->data()[b] - inPs->data()[a]) * t) : 1.0f;
                        if (oCd && inCd)
                            oCd->data()[outIdx] = inCd->data()[a] + (inCd->data()[b] - inCd->data()[a]) * t;
                    };

                    // Open: include both endpoints (denominator count-1).
                    // Closed: evenly around the loop without duplicating the seam.
                    const float denom = closed ? static_cast<float>(count)
                                               : static_cast<float>(count - 1);
                    for (int k = 0; k < count; ++k)
                    {
                        float s = total * (static_cast<float>(k) / denom);
                        if (!closed && k == count - 1) s = total; // land exactly on the end
                        size_t idx = static_cast<size_t>(k);
                        sampleAt(s, idx);
                    }

                    mograph::setCurveClosed(out, closed);
                    if (recomputeFrames)
                        mograph::writeCurveFrames(out, closed);
                    return out;
                }
            };
        }

        void registerResampleSop()
        {
            SopRegistry::instance().registerType(
                {"resample", "Resample", "Modifiers",
                 /*inputs*/ {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     ParamSpec{"mode", ParamType::String, "\"count\"", 0.0, 0.0, 0.0, {"count", "length"}},
                     ParamSpec{"count", ParamType::Int, "50", 2.0, 5000.0, 1.0},
                     ParamSpec{"segment_length", ParamType::Float, "0.5", 0.001, 20.0, 0.001},
                     ParamSpec{"recompute_frames", ParamType::Bool, "true"},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<ResampleSop>(uid);
                });
        }
    }
}
