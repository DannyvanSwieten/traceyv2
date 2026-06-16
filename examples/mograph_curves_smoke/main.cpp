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

// Cook a 1-input modifier node over `input`.
Geometry cookMod(const char *kind, const Geometry &input,
                 const std::function<void(SopNode &)> &setup = {})
{
    auto node = SopRegistry::instance().create(kind, 2);
    if (!node) return {};
    if (setup) setup(*node);
    const Geometry *in = &input;
    std::vector<const Geometry *> inputs{in};
    return node->cook(inputs);
}

// Cook a 2-input node over (a, b).
Geometry cook2(const char *kind, const Geometry &a, const Geometry &b,
               const std::function<void(SopNode &)> &setup = {})
{
    auto node = SopRegistry::instance().create(kind, 3);
    if (!node) return {};
    if (setup) setup(*node);
    const Geometry *pa = &a;
    const Geometry *pb = &b;
    std::vector<const Geometry *> inputs{pa, pb};
    return node->cook(inputs);
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

    // ── Resample (line: exact even spacing) ──────────────────────────
    std::printf("[resample line]\n");
    {
        Geometry line = cookGen("line", [](SopNode &n) {
            n.setParamVec3("start", Vec3(-3.0f, 0.0f, 0.0f));
            n.setParamVec3("end", Vec3(3.0f, 0.0f, 0.0f));
            n.setParamInt("points", 4);
        });
        Geometry g = cookMod("resample", line, [](SopNode &n) {
            n.setParamString("mode", "count");
            n.setParamInt("count", 7);
        });
        check(g.pointCount() == 7, "resampled to 7 points");
        const auto &P = g.positions();
        if (P.size() == 7)
        {
            check(approxEq(P.front().x, -3.0f) && approxEq(P.back().x, 3.0f), "endpoints preserved");
            bool even = true;
            for (size_t i = 0; i + 1 < P.size(); ++i)
                if (!approxEq(P[i + 1].x - P[i].x, 1.0f)) even = false;
            check(even, "even 1.0 spacing along the line");
        }
    }

    // ── Resample (closed circle: even arc spacing, closed preserved) ──
    std::printf("[resample circle]\n");
    {
        Geometry circ = cookGen("circle", [](SopNode &n) {
            n.setParamFloat("radius", 2.0f);
            n.setParamInt("segments", 12);
        });
        Geometry g = cookMod("resample", circ, [](SopNode &n) {
            n.setParamString("mode", "count");
            n.setParamInt("count", 24);
        });
        check(g.pointCount() == 24, "resampled closed ring to 24 points");
        check(mograph::curveClosed(g), "closed flag preserved");
        const auto &P = g.positions();
        if (P.size() == 24)
        {
            float mn = 1e9f, mx = 0.0f;
            for (size_t i = 0; i < P.size(); ++i)
            {
                float d = glm::length(P[(i + 1) % P.size()] - P[i]);
                mn = std::min(mn, d);
                mx = std::max(mx, d);
            }
            check(mx / std::max(mn, 1e-6f) < 1.1f, "roughly uniform arc spacing");
        }
    }

    // ── Resample by length ───────────────────────────────────────────
    std::printf("[resample length]\n");
    {
        Geometry line = cookGen("line", [](SopNode &n) {
            n.setParamVec3("start", Vec3(0.0f, 0.0f, 0.0f));
            n.setParamVec3("end", Vec3(10.0f, 0.0f, 0.0f));
            n.setParamInt("points", 2);
        });
        Geometry g = cookMod("resample", line, [](SopNode &n) {
            n.setParamString("mode", "length");
            n.setParamFloat("segment_length", 1.0f);
        });
        check(g.pointCount() == 11, "length 10 / seg 1 → 11 points");
    }

    // ── Sweep (circle profile along a vertical line → tube) ──────────
    std::printf("[sweep]\n");
    {
        Geometry profile = cookGen("circle", [](SopNode &n) {
            n.setParamFloat("radius", 0.3f);
            n.setParamInt("segments", 8);
        }); // 8 closed points
        Geometry pathLine = cookGen("line", [](SopNode &n) {
            n.setParamVec3("start", Vec3(0.0f, 0.0f, 0.0f));
            n.setParamVec3("end", Vec3(0.0f, 4.0f, 0.0f));
            n.setParamInt("points", 5);
        }); // 5 open points along +Y

        Geometry tube = cook2("sweep", pathLine, profile, [](SopNode &n) {
            n.setParamBool("caps", true);
        });
        // 5 rings × 8 profile points = 40, + 2 cap centroids
        check(tube.pointCount() == 42, "tube has 40 grid + 2 cap points");
        // walls 4 spans × 8 × 2 = 64, caps 2 ends × 8 = 16 → 80 tris
        check(tube.primitiveCount() == 80, "80 triangles (walls + caps)");

        const auto &P = tube.positions();
        const auto *N = tube.points().get<Vec3>("N");
        bool onRadius = true;
        for (size_t k = 0; k < 40; ++k) // grid points only
        {
            const float r = std::sqrt(P[k].x * P[k].x + P[k].z * P[k].z);
            if (!approxEq(r, 0.3f, 1e-3f)) onRadius = false;
        }
        check(onRadius, "all wall points at profile radius 0.3 from the path axis");

        // Outward normal: N points away from the Y axis for a grid point.
        bool nOutward = true;
        if (N)
            for (size_t k = 0; k < 40; ++k)
            {
                glm::vec3 radial = glm::normalize(glm::vec3(P[k].x, 0.0f, P[k].z));
                if (glm::dot(radial, N->data()[k]) < 0.9f) nOutward = false;
            }
        check(N && nOutward, "smooth normals point radially outward");

        // Winding: the first wall triangle's geometric normal must agree with
        // outward (cross(P1-P0,P2-P0) is the engine's front-face normal).
        const auto &prims = tube.primitivesList();
        const auto &v2p = tube.vertexToPoint();
        if (!prims.empty())
        {
            const auto &pr = prims[0];
            const glm::vec3 p0 = P[v2p[pr.firstVertex]];
            const glm::vec3 p1 = P[v2p[pr.firstVertex + 1]];
            const glm::vec3 p2 = P[v2p[pr.firstVertex + 2]];
            const glm::vec3 gn = glm::cross(p1 - p0, p2 - p0);
            const glm::vec3 center = (p0 + p1 + p2) / 3.0f;
            const glm::vec3 radial = glm::normalize(glm::vec3(center.x, 0.0f, center.z));
            check(glm::dot(glm::normalize(gn), radial) > 0.5f,
                  "triangle winding yields outward-facing front normal");
        }

        // caps off → fewer points/tris
        Geometry open = cook2("sweep", pathLine, profile, [](SopNode &n) {
            n.setParamBool("caps", false);
        });
        check(open.pointCount() == 40, "no-caps tube has 40 points");
        check(open.primitiveCount() == 64, "no-caps tube has 64 wall triangles");
    }

    if (failures == 0) std::printf("[mograph_curves_smoke] all checks passed\n");
    else               std::printf("[mograph_curves_smoke] %d failure(s)\n", failures);
    return failures > 0 ? 1 : 0;
}
