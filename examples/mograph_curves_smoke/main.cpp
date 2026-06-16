// Smoke test for MoGraph Phase 2 curve generators (Line / Circle / Spiral)
// and the rotation-minimizing frames they bake into N + orient.
//
// A "curve" is an ordered point cloud (point index = path order) + a Detail
// `closed` flag + per-point `orient` (Vec4 wxyz) whose local +Z aims along
// the path tangent. These asserts validate that contract — which is exactly
// what clone-along-spline relies on (curve.orient → cloner).
//
// Exit 0 on success. Depends only on `tracey` — no Vulkan, no rendering.

#include "geometry/geometry.hpp"
#include "geometry/attribute.hpp"
#include "geometry/attribute_table.hpp"

#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"
#include "sops/mograph/orient_util.hpp"
#include "sops/mograph/curve_frame.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char *what)
{
    if (ok) std::printf("  ok   %s\n", what);
    else { ++failures; std::printf("  FAIL %s\n", what); }
}
bool approxEq(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

using namespace tracey;
using namespace tracey::sops;

// Cook a generator node standalone (no inputs) and return its Geometry.
Geometry cookGen(const char *kind, const std::function<void(SopNode &)> &setup = {})
{
    auto node = SopRegistry::instance().create(kind, 1);
    if (!node) return {};
    if (setup) setup(*node);
    return node->cook({});
}

// World tangent = orient rotates local +Z.
glm::vec3 tangentOf(const Vec4 &orient)
{
    return mograph::mat3FromWxyz(orient) * glm::vec3(0.0f, 0.0f, 1.0f);
}

} // namespace

int main()
{
    SopRegistry::instance();
    registerBuiltinSops();

    // ── Line ──────────────────────────────────────────────────────────
    std::printf("[line]\n");
    {
        Geometry g = cookGen("line", [](SopNode &n) {
            n.setParamVec3("start", Vec3(-2.0f, 0.0f, 0.0f));
            n.setParamVec3("end", Vec3(2.0f, 0.0f, 0.0f));
            n.setParamInt("points", 5);
        });
        check(g.pointCount() == 5, "5 points");
        const auto &P = g.positions();
        check(!P.empty() && approxEq(P.front().x, -2.0f), "first point at start");
        check(!P.empty() && approxEq(P.back().x, 2.0f), "last point at end");
        check(P.size() == 5 && approxEq(P[2].x, 0.0f), "midpoint at origin");
        check(!mograph::curveClosed(g), "line is open");
        const auto *orient = g.points().get<Vec4>("orient");
        check(orient != nullptr, "orient attribute present");
        if (orient && orient->data().size() == 5)
        {
            // line runs along +X, so the tangent (local +Z mapped) ≈ +X.
            glm::vec3 t = tangentOf(orient->data()[2]);
            check(approxEq(t.x, 1.0f) && approxEq(t.y, 0.0f) && approxEq(t.z, 0.0f),
                  "tangent points along +X");
            // unit quaternion
            const Vec4 q = orient->data()[2];
            check(approxEq(std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w), 1.0f),
                  "orient is unit quaternion");
        }
    }

    // ── Circle (closed) ──────────────────────────────────────────────
    std::printf("[circle]\n");
    {
        Geometry g = cookGen("circle", [](SopNode &n) {
            n.setParamFloat("radius", 2.0f);
            n.setParamInt("segments", 8);
            n.setParamFloat("arc", 360.0f);
        });
        check(g.pointCount() == 8, "8 points (closed ring of 8 segments)");
        check(mograph::curveClosed(g), "full circle is closed");
        const auto &P = g.positions();
        bool onPlane = true, onRadius = true;
        for (const auto &p : P)
        {
            if (!approxEq(p.y, 0.0f)) onPlane = false;
            if (!approxEq(std::sqrt(p.x*p.x + p.z*p.z), 2.0f)) onRadius = false;
        }
        check(onPlane, "all points on XZ plane (y=0)");
        check(onRadius, "all points at radius 2");
        const auto *orient = g.points().get<Vec4>("orient");
        check(orient != nullptr && orient->data().size() == 8, "orient present per point");
        if (orient && orient->data().size() == 8)
        {
            // For a circle the tangent is perpendicular to the radial direction.
            bool tangential = true;
            for (size_t i = 0; i < 8; ++i)
            {
                glm::vec3 radial = glm::normalize(glm::vec3(P[i].x, 0.0f, P[i].z));
                glm::vec3 t = tangentOf(orient->data()[i]);
                if (std::fabs(glm::dot(radial, t)) > 0.2f) tangential = false;
            }
            check(tangential, "tangents perpendicular to radius (frame follows ring)");
        }
    }

    // ── Circle arc (open) ────────────────────────────────────────────
    std::printf("[circle arc]\n");
    {
        Geometry g = cookGen("circle", [](SopNode &n) {
            n.setParamInt("segments", 8);
            n.setParamFloat("arc", 180.0f);
        });
        check(g.pointCount() == 9, "arc has segments+1 points");
        check(!mograph::curveClosed(g), "180° arc is open");
    }

    // ── Spiral ───────────────────────────────────────────────────────
    std::printf("[spiral]\n");
    {
        Geometry g = cookGen("spiral", [](SopNode &n) {
            n.setParamFloat("radius", 1.0f);
            n.setParamFloat("end_radius", 1.0f);
            n.setParamFloat("height", 4.0f);
            n.setParamFloat("turns", 2.0f);
            n.setParamInt("points", 33);
        });
        check(g.pointCount() == 33, "33 points");
        const auto &P = g.positions();
        check(!P.empty() && approxEq(P.front().y, 0.0f), "starts at y=0");
        check(!P.empty() && approxEq(P.back().y, 4.0f), "rises to height");
        bool onRadius = true;
        for (const auto &p : P)
            if (!approxEq(std::sqrt(p.x*p.x + p.z*p.z), 1.0f, 1e-2f)) onRadius = false;
        check(onRadius, "constant radius 1");
        const auto *orient = g.points().get<Vec4>("orient");
        check(orient != nullptr, "orient present");
        if (orient)
        {
            bool allUnit = true;
            for (const auto &q : orient->data())
                if (!approxEq(std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w), 1.0f)) allUnit = false;
            check(allUnit, "all orients unit quaternions");
        }
    }

    if (failures == 0) std::printf("[mograph_curves_smoke] all checks passed\n");
    else               std::printf("[mograph_curves_smoke] %d failure(s)\n", failures);
    return failures > 0 ? 1 : 0;
}
