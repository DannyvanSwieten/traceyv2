// Smoke test for the cloner SOP trio: points_grid, copy_to_points, scatter.
//
// Exercises:
//   1. points_grid: 2×2×2 with spacing 1 → 8 points at the expected
//      lattice positions, `N=+Y` and `pscale=1` filled in.
//   2. copy_to_points: cube → copy_to_points ← points_grid (2×2×2) →
//      object_output. One EmittedActor with 8 × 36 = 288 vertices.
//   3. Per-instance Cd transfer: a hand-built 3-point template carrying
//      distinct Cd values → output has a vertex-class Cd attribute with
//      the right per-clone bands (first 36 verts = template[0].Cd, etc.).
//   4. scatter: plane (Y=0) → scatter(count=50, seed=42) → 50 points on
//      Y≈0, deterministic across re-runs with the same seed.
//
// Exit 0 on success. Depends only on `tracey` — no Vulkan, no rendering.

#include "geometry/geometry.hpp"
#include "geometry/attribute.hpp"
#include "geometry/attribute_table.hpp"
#include "geometry/geometry_converter.hpp"
#include "scene/scene_object.hpp"

#include "sops/sop_graph.hpp"
#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (ok) std::printf("  ok   %s\n", what);
    else { ++failures; std::printf("  FAIL %s\n", what); }
}

bool approxEq(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

}

int main()
{
    using namespace tracey;
    using namespace tracey::sops;

    SopRegistry::instance();
    registerBuiltinSops();

    // ───────────────────────────────────────────────────────────────────
    // 1) points_grid standalone: 2×2×2 with spacing 1.
    // ───────────────────────────────────────────────────────────────────
    {
        SopGraph g(0);
        auto grid = SopRegistry::instance().create("points_grid", g.nextUid());
        check(grid != nullptr, "create points_grid");
        grid->setParamInt("count_x", 2);
        grid->setParamInt("count_y", 2);
        grid->setParamInt("count_z", 2);
        grid->setParamFloat("spacing_x", 1.0f);
        grid->setParamFloat("spacing_y", 1.0f);
        grid->setParamFloat("spacing_z", 1.0f);
        const size_t gridUid = grid->uid();
        g.addNode(std::move(grid));

        auto out = SopRegistry::instance().create("object_output", g.nextUid());
        out->setParamString("name", "grid");
        const size_t outUid = out->uid();
        g.addNode(std::move(out));
        g.createConnection(gridUid, 0, outUid, 0);

        CookDiagnostic diag;
        auto emitted = g.cook(&diag);
        check(diag.ok, "points_grid cook ok");
        check(emitted.size() == 1, "one EmittedActor from grid");
        if (emitted.empty()) return 1;
        const auto &geo = emitted.front().geometry;
        check(geo.pointCount() == 8, "8 points emitted (2*2*2)");
        const auto *N  = geo.points().get<Vec3>("N");
        const auto *ps = geo.points().get<float>("pscale");
        check(N != nullptr && ps != nullptr, "N and pscale attributes present");
        if (N && ps)
        {
            bool allUp = true;
            bool allOne = true;
            for (size_t i = 0; i < geo.pointCount(); ++i)
            {
                if (!(approxEq(N->data()[i].x, 0.0f) &&
                      approxEq(N->data()[i].y, 1.0f) &&
                      approxEq(N->data()[i].z, 0.0f))) allUp = false;
                if (!approxEq(ps->data()[i], 1.0f)) allOne = false;
            }
            check(allUp,  "every grid point has N = +Y");
            check(allOne, "every grid point has pscale = 1");
        }
        // Centered on origin: 2 points per axis at -0.5, +0.5.
        bool bounds_ok = true;
        for (const auto &p : geo.positions())
        {
            if (!(approxEq(std::fabs(p.x), 0.5f) &&
                  approxEq(std::fabs(p.y), 0.5f) &&
                  approxEq(std::fabs(p.z), 0.5f)))
            {
                bounds_ok = false;
                break;
            }
        }
        check(bounds_ok, "grid points centered on origin (±0.5 per axis)");
    }

    // ───────────────────────────────────────────────────────────────────
    // 2) copy_to_points: cube + 2×2×2 grid.
    // ───────────────────────────────────────────────────────────────────
    {
        SopGraph g(0);
        auto cube = SopRegistry::instance().create("primitive_cube", g.nextUid());
        cube->setParamFloat("size", 0.4f); // smaller than spacing so cubes don't merge
        const size_t cubeUid = cube->uid();
        g.addNode(std::move(cube));

        auto grid = SopRegistry::instance().create("points_grid", g.nextUid());
        grid->setParamInt("count_x", 2);
        grid->setParamInt("count_y", 2);
        grid->setParamInt("count_z", 2);
        const size_t gridUid = grid->uid();
        g.addNode(std::move(grid));

        auto copy = SopRegistry::instance().create("copy_to_points", g.nextUid());
        const size_t copyUid = copy->uid();
        g.addNode(std::move(copy));

        auto out = SopRegistry::instance().create("object_output", g.nextUid());
        out->setParamString("name", "clones");
        const size_t outUid = out->uid();
        g.addNode(std::move(out));

        g.createConnection(cubeUid, 0, copyUid, 0);  // stamp
        g.createConnection(gridUid, 0, copyUid, 1);  // template
        g.createConnection(copyUid, 0, outUid, 0);

        CookDiagnostic diag;
        auto emitted = g.cook(&diag);
        check(diag.ok, "cube+grid copy cook ok");
        check(emitted.size() == 1, "one EmittedActor from copy_to_points");
        if (emitted.empty()) return 1;
        const auto &geo = emitted.front().geometry;
        // Cube has 36 verts (non-indexed: 12 tris × 3). 8 clones → 288.
        check(geo.vertexCount() == 8 * 36, "288 vertices (8 cubes × 36)");
        check(geo.primitiveCount() == 8 * 12, "96 triangles (8 cubes × 12)");
    }

    // ───────────────────────────────────────────────────────────────────
    // 3) Per-instance Cd transfer.
    // ───────────────────────────────────────────────────────────────────
    {
        // Build a tiny 3-point template by injecting it directly via a
        // synthetic generator node would be over-engineering; instead, drive
        // the copy_to_points node directly with hand-built Geometry inputs.
        Geometry stamp = GeometryConverter::fromSceneObject(SceneObject::createCube(0.5f));

        Geometry tmpl;
        auto &pts = tmpl.points();
        auto *P  = pts.add<Vec3>("P",  Vec3(0.0f));
        auto *Cd = pts.add<Vec3>("Cd", Vec3(1.0f));
        tmpl.resizePoints(3);
        P->data()[0]  = Vec3(0, 0, 0); Cd->data()[0] = Vec3(1, 0, 0);  // red
        P->data()[1]  = Vec3(2, 0, 0); Cd->data()[1] = Vec3(0, 1, 0);  // green
        P->data()[2]  = Vec3(4, 0, 0); Cd->data()[2] = Vec3(0, 0, 1);  // blue

        // Construct the SOP node directly (no graph topology needed — cook is pure).
        auto copy = SopRegistry::instance().create("copy_to_points", 0);
        const Geometry *inputs[] = {&stamp, &tmpl};
        Geometry out = copy->cook(
            std::span<const Geometry *const>{inputs, 2});

        check(out.vertexCount() == 3 * 36, "Cd transfer: 108 verts (3 cubes × 36)");
        const auto *cv = out.vertices().get<Vec3>("Cd");
        check(cv != nullptr, "Cd transfer: per-vertex Cd present on output");
        if (cv)
        {
            bool band_ok = true;
            // mergeFrom preserves insertion order, so the first clone's
            // vertices land at [0, 36), second at [36, 72), third at [72, 108).
            for (size_t i = 0; i < 36; ++i)
            {
                if (!(approxEq(cv->data()[i].x, 1.0f) &&
                      approxEq(cv->data()[i].y, 0.0f) &&
                      approxEq(cv->data()[i].z, 0.0f))) { band_ok = false; break; }
            }
            for (size_t i = 36; i < 72 && band_ok; ++i)
            {
                if (!(approxEq(cv->data()[i].x, 0.0f) &&
                      approxEq(cv->data()[i].y, 1.0f) &&
                      approxEq(cv->data()[i].z, 0.0f))) { band_ok = false; break; }
            }
            for (size_t i = 72; i < 108 && band_ok; ++i)
            {
                if (!(approxEq(cv->data()[i].x, 0.0f) &&
                      approxEq(cv->data()[i].y, 0.0f) &&
                      approxEq(cv->data()[i].z, 1.0f))) { band_ok = false; break; }
            }
            check(band_ok, "Cd transfer: each clone's verts carry the template point's Cd");
        }
    }

    // ───────────────────────────────────────────────────────────────────
    // 4) Scatter on a plane: 50 points on Y≈0, deterministic with seed.
    // ───────────────────────────────────────────────────────────────────
    {
        Geometry plane = GeometryConverter::fromSceneObject(
            SceneObject::createPlane(4.0f, 4.0f, 1, 1));

        auto run = [&plane]() {
            auto sc = SopRegistry::instance().create("scatter", 0);
            sc->setParamInt("count", 50);
            sc->setParamInt("seed", 42);
            const Geometry *inputs[] = {&plane};
            return sc->cook(std::span<const Geometry *const>{inputs, 1});
        };

        Geometry a = run();
        Geometry b = run();

        check(a.pointCount() == 50, "scatter emitted 50 points");
        bool on_plane = true;
        for (const auto &p : a.positions())
        {
            if (std::fabs(p.y) > 1e-4f) { on_plane = false; break; }
        }
        check(on_plane, "all scattered points lie on the plane (y≈0)");

        bool deterministic = (a.pointCount() == b.pointCount());
        if (deterministic)
        {
            for (size_t i = 0; i < a.pointCount(); ++i)
            {
                const Vec3 &pa = a.positions()[i];
                const Vec3 &pb = b.positions()[i];
                if (!(approxEq(pa.x, pb.x) && approxEq(pa.y, pb.y) &&
                      approxEq(pa.z, pb.z)))
                {
                    deterministic = false;
                    break;
                }
            }
        }
        check(deterministic, "scatter is deterministic given the same seed");
    }

    if (failures == 0) std::printf("[cloners_smoke] all checks passed\n");
    else               std::printf("[cloners_smoke] %d failure(s)\n", failures);
    return failures > 0 ? 1 : 0;
}
