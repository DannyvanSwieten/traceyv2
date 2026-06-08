// BoundSop — emit the axis-aligned bounding box of the input geometry
// as a triangulated cube. Useful as a quick visualisation aid (drop
// it after a heavy SOP to see the footprint), as a Cookie Cutter for
// downstream Delete (wire Bound → Transform → use as Delete's bbox),
// or as a generator in its own right (small fixed-size scene markers).
//
// `padding` expands the bbox uniformly along all six faces, so a
// padding of 0.1 around a unit cube produces a 1.2-cube. Negative
// values shrink (useful for inset markers).

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../geometry/geometry.hpp"

#include <algorithm>
#include <limits>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class BoundSop : public SopNode
            {
            public:
                explicit BoundSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeFloat("padding", 0.0f));
                }
                std::string kind() const override { return "bound"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput (PortInfo::createInput ("in",  DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    Geometry out;
                    if (inputs.empty() || !inputs[0]) return out;
                    const auto &P = inputs[0]->positions();
                    if (P.empty()) return out;

                    Vec3 lo( std::numeric_limits<float>::infinity());
                    Vec3 hi(-std::numeric_limits<float>::infinity());
                    for (const auto &p : P)
                    {
                        lo.x = std::min(lo.x, p.x); hi.x = std::max(hi.x, p.x);
                        lo.y = std::min(lo.y, p.y); hi.y = std::max(hi.y, p.y);
                        lo.z = std::min(lo.z, p.z); hi.z = std::max(hi.z, p.z);
                    }
                    const float pad = paramFloat("padding", 0.0f);
                    lo.x -= pad; lo.y -= pad; lo.z -= pad;
                    hi.x += pad; hi.y += pad; hi.z += pad;

                    // 8 corner points, indexed by (x, y, z) bit pattern so
                    // the face winding below is easy to read.
                    const Vec3 corners[8] = {
                        Vec3(lo.x, lo.y, lo.z),  // 0
                        Vec3(hi.x, lo.y, lo.z),  // 1
                        Vec3(lo.x, hi.y, lo.z),  // 2
                        Vec3(hi.x, hi.y, lo.z),  // 3
                        Vec3(lo.x, lo.y, hi.z),  // 4
                        Vec3(hi.x, lo.y, hi.z),  // 5
                        Vec3(lo.x, hi.y, hi.z),  // 6
                        Vec3(hi.x, hi.y, hi.z),  // 7
                    };
                    for (int i = 0; i < 8; ++i) out.addPoint(corners[i]);

                    // Six faces, two triangles each. Winding is CCW when
                    // viewed from outside so normals point outward (matches
                    // primitive_cube's convention — kept identical so a
                    // Bound-and-render pass renders the same way as a
                    // primitive cube).
                    auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
                        out.addTriangle(a, b, c);
                        out.addTriangle(a, c, d);
                    };
                    // -Z (back)   +Z (front)
                    quad(0, 2, 3, 1);
                    quad(4, 5, 7, 6);
                    // -X (left)   +X (right)
                    quad(0, 4, 6, 2);
                    quad(1, 3, 7, 5);
                    // -Y (bottom) +Y (top)
                    quad(0, 1, 5, 4);
                    quad(2, 6, 7, 3);
                    return out;
                }
            };
        }  // anon

        void registerBoundSop()
        {
            SopRegistry::instance().registerType(
                {"bound", "Bound", "Modifiers",
                 /*inputs*/  {{"in"}}, /*outputs*/ {{"out"}},
                 /*params*/  {{"padding", ParamType::Float, "0.0"}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<BoundSop>(uid);
                });
        }
    }
}
