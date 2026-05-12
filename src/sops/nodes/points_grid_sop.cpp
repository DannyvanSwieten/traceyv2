// Generator SOP: a regular N×M×K grid of points centered on the origin.
// No primitives — pure point cloud, intended as the template input for
// copy_to_points and other cloning SOPs.
//
// Each point gets the conventional cloner attributes pre-filled:
//   • P      (Vec3) — grid position
//   • N      (Vec3) — +Y (so copy_to_points doesn't rotate by default)
//   • pscale (float) — 1.0
//
// This matches Houdini's convention so cloner SOPs that read N/pscale
// behave the same way they do over there.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

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
            class PointsGridSop : public SopNode
            {
            public:
                explicit PointsGridSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeInt("count_x", 3));
                    declareParam(Parameter::makeInt("count_y", 3));
                    declareParam(Parameter::makeInt("count_z", 3));
                    declareParam(Parameter::makeFloat("spacing_x", 1.0f));
                    declareParam(Parameter::makeFloat("spacing_y", 1.0f));
                    declareParam(Parameter::makeFloat("spacing_z", 1.0f));
                }

                std::string kind() const override { return "points_grid"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    const int cx = std::max(1, paramInt("count_x", 3));
                    const int cy = std::max(1, paramInt("count_y", 3));
                    const int cz = std::max(1, paramInt("count_z", 3));
                    const float sx = paramFloat("spacing_x", 1.0f);
                    const float sy = paramFloat("spacing_y", 1.0f);
                    const float sz = paramFloat("spacing_z", 1.0f);

                    Geometry g;
                    auto &pts = g.points();
                    auto *P  = pts.add<Vec3>("P", Vec3(0.0f));
                    auto *N  = pts.add<Vec3>("N", Vec3(0.0f, 1.0f, 0.0f));
                    auto *ps = pts.add<float>("pscale", 1.0f);

                    const size_t total = static_cast<size_t>(cx) *
                                         static_cast<size_t>(cy) *
                                         static_cast<size_t>(cz);
                    g.resizePoints(total);

                    // Center the grid on the origin: shift each axis by
                    // -((count-1)/2) * spacing.
                    const float ox = -(cx - 1) * 0.5f * sx;
                    const float oy = -(cy - 1) * 0.5f * sy;
                    const float oz = -(cz - 1) * 0.5f * sz;

                    auto &Pd  = P->data();
                    auto &Nd  = N->data();
                    auto &psd = ps->data();

                    size_t i = 0;
                    for (int z = 0; z < cz; ++z)
                    {
                        for (int y = 0; y < cy; ++y)
                        {
                            for (int x = 0; x < cx; ++x, ++i)
                            {
                                Pd[i]  = Vec3(ox + x * sx, oy + y * sy, oz + z * sz);
                                Nd[i]  = Vec3(0.0f, 1.0f, 0.0f);
                                psd[i] = 1.0f;
                            }
                        }
                    }
                    return g;
                }
            };
        }

        void registerPointsGridSop()
        {
            SopRegistry::instance().registerType(
                {"points_grid", "Points Grid", "Cloners",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {
                     {"count_x",   ParamType::Int,   "3"},
                     {"count_y",   ParamType::Int,   "3"},
                     {"count_z",   ParamType::Int,   "3"},
                     {"spacing_x", ParamType::Float, "1.0"},
                     {"spacing_y", ParamType::Float, "1.0"},
                     {"spacing_z", ParamType::Float, "1.0"},
                 }},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<PointsGridSop>(uid);
                });
        }
    }
}
