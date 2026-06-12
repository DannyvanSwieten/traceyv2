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
#include "geometry/attribute_allocator.hpp"
#include "geometry/geometry_converter.hpp"
#include "scene/scene_object.hpp"

#include "sops/sop_graph.hpp"
#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"
#include "sops/codegen/copy_to_points_compute.hpp"

#include "device/device.hpp"

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
        if (!emitted.front().geometry) { std::printf("FAIL: emitted.geometry null\n"); return 1; }
        const auto &geo = *emitted.front().geometry;
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
        if (!emitted.front().geometry) { std::printf("FAIL: emitted.geometry null\n"); return 1; }
        const auto &geo = *emitted.front().geometry;
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

    // ───────────────────────────────────────────────────────────────────
    // 5) GPU copy_to_points: same cube+grid scenario as test (2), routed
    //    through the CopyToPointsCompute dispatcher. Validates equivalence
    //    on the standard particle case (P, N, uv on stamp; P on template).
    //    Silently skipped when no Vulkan device is available.
    // ───────────────────────────────────────────────────────────────────
    {
        std::unique_ptr<tracey::Device> device;
        try
        {
            device.reset(tracey::createDevice(tracey::DeviceType::Gpu,
                                              tracey::DeviceBackend::Compute));
        }
        catch (const std::exception &e)
        {
            std::printf("\n[gpu] device unavailable (%s) — skipping CTP GPU compare\n",
                        e.what());
        }
        if (device)
        {
            tracey::AttributeAllocator::setDevice(device.get());
            tracey::sops::codegen::CopyToPointsCompute gpu(device.get());
            tracey::sops::codegen::CopyToPointsCompute::setGlobal(&gpu);

            std::printf("\n── CTP CPU↔GPU equivalence ──\n");

            Geometry stamp = GeometryConverter::fromSceneObject(SceneObject::createCube(0.4f));
            Geometry tmpl;
            {
                auto &pts = tmpl.points();
                auto *P = pts.add<Vec3>("P", Vec3(0.0f));
                pts.add<Vec3>("N", Vec3(0, 1, 0));
                pts.add<float>("pscale", 1.0f);
                tmpl.resizePoints(8);
                size_t k = 0;
                for (int z = 0; z < 2; ++z)
                    for (int y = 0; y < 2; ++y)
                        for (int x = 0; x < 2; ++x)
                            P->data()[k++] = Vec3(float(x) - 0.5f,
                                                  float(y) - 0.5f,
                                                  float(z) - 0.5f);
            }

            auto copy = SopRegistry::instance().create("copy_to_points", 0);
            const Geometry *inputs[] = {&stamp, &tmpl};

            Geometry gpu_out = copy->cook(
                std::span<const Geometry *const>{inputs, 2});
            check(gpu_out.vertexCount() == 8 * 36, "GPU: 288 vertices (8 cubes × 36)");
            check(gpu_out.primitiveCount() == 8 * 12, "GPU: 96 triangles");

            // Deregister to force CPU path for the reference.
            tracey::sops::codegen::CopyToPointsCompute::setGlobal(nullptr);
            Geometry cpu_out = copy->cook(
                std::span<const Geometry *const>{inputs, 2});

            check(cpu_out.vertexCount() == gpu_out.vertexCount(),
                  "GPU vs CPU: vertex counts match");
            const auto &gP = gpu_out.positions();
            const auto &cP = cpu_out.positions();
            bool p_match = (gP.size() == cP.size());
            for (size_t i = 0; i < gP.size() && p_match; ++i)
            {
                if (!(approxEq(gP[i].x, cP[i].x) &&
                      approxEq(gP[i].y, cP[i].y) &&
                      approxEq(gP[i].z, cP[i].z)))
                {
                    p_match = false;
                    std::printf("    P[%zu]: gpu=(%.4f,%.4f,%.4f) cpu=(%.4f,%.4f,%.4f)\n",
                                i, gP[i].x, gP[i].y, gP[i].z, cP[i].x, cP[i].y, cP[i].z);
                }
            }
            check(p_match, "GPU vs CPU: per-vertex P matches");

            const auto *gN = gpu_out.points().get<Vec3>("N");
            const auto *cN = cpu_out.points().get<Vec3>("N");
            check(gN && cN, "GPU vs CPU: both produced N");
            if (gN && cN)
            {
                bool n_match = (gN->data().size() == cN->data().size());
                for (size_t i = 0; i < gN->data().size() && n_match; ++i)
                {
                    const Vec3 &a = gN->data()[i];
                    const Vec3 &b = cN->data()[i];
                    if (!(approxEq(a.x, b.x) && approxEq(a.y, b.y) &&
                          approxEq(a.z, b.z))) n_match = false;
                }
                check(n_match, "GPU vs CPU: per-vertex N matches");
            }

            tracey::AttributeAllocator::setDevice(nullptr);
        }
    }

    // ───────────────────────────────────────────────────────────────────
    // 6) plain_effector: sphere falloff weights position offset + Cd debug.
    // ───────────────────────────────────────────────────────────────────
    {
        // Template: 3 points along X at 0 / 2 / 4. Sphere falloff of radius 1
        // at the origin covers point 0 fully, leaves 1 and 2 untouched.
        Geometry tmpl;
        auto *P = tmpl.points().add<Vec3>("P", Vec3(0.0f));
        tmpl.resizePoints(3);
        P->data()[0] = Vec3(0, 0, 0);
        P->data()[1] = Vec3(2, 0, 0);
        P->data()[2] = Vec3(4, 0, 0);

        auto fx = SopRegistry::instance().create("plain_effector", 0);
        check(fx != nullptr, "create plain_effector");
        fx->setParamVec3("position", Vec3(0.0f, 1.0f, 0.0f));
        fx->setParamString("falloff_shape", "sphere");
        fx->setParamVec3("falloff_center", Vec3(0.0f));
        fx->setParamVec3("falloff_size", Vec3(1.0f));
        fx->setParamBool("weight_to_cd", true);

        const Geometry *inputs[] = {&tmpl};
        Geometry out = fx->cook(std::span<const Geometry *const>{inputs, 1});

        const auto &op = out.positions();
        check(op.size() == 3, "plain_effector: 3 points out");
        check(approxEq(op[0].y, 1.0f), "plain_effector: point inside falloff moved +Y");
        check(approxEq(op[1].y, 0.0f) && approxEq(op[2].y, 0.0f),
              "plain_effector: points outside falloff unmoved");

        const auto *cd = out.points().get<Vec3>("Cd");
        check(cd != nullptr, "plain_effector: weight_to_cd created Cd");
        if (cd)
        {
            check(approxEq(cd->data()[0].x, 1.0f) && approxEq(cd->data()[1].x, 0.0f),
                  "plain_effector: Cd carries falloff weight (1 inside, 0 outside)");
        }

        // Inner-band smoothstep: a point at half radius with inner=0 sits in
        // the smoothstep mid-band — strictly between 0 and 1.
        P->data()[1] = Vec3(0.5f, 0.0f, 0.0f);
        Geometry out2 = fx->cook(std::span<const Geometry *const>{inputs, 1});
        const float midY = out2.positions()[1].y;
        check(midY > 0.05f && midY < 0.95f,
              "plain_effector: smoothstep mid-band weight between 0 and 1");

        // strength=0 ⇒ identity passthrough.
        fx->setParamFloat("strength", 0.0f);
        fx->setParamBool("weight_to_cd", false);
        Geometry out3 = fx->cook(std::span<const Geometry *const>{inputs, 1});
        check(approxEq(out3.positions()[0].y, 0.0f),
              "plain_effector: strength=0 is a passthrough");
    }

    // ───────────────────────────────────────────────────────────────────
    // 7) random_effector: deterministic per (index, seed).
    // ───────────────────────────────────────────────────────────────────
    {
        Geometry tmpl;
        auto *P = tmpl.points().add<Vec3>("P", Vec3(0.0f));
        tmpl.resizePoints(8);
        for (size_t i = 0; i < 8; ++i)
            P->data()[i] = Vec3(static_cast<float>(i), 0.0f, 0.0f);

        auto run = [&tmpl](int seed) {
            auto fx = SopRegistry::instance().create("random_effector", 0);
            fx->setParamInt("seed", seed);
            fx->setParamVec3("position_amount", Vec3(0.5f, 0.5f, 0.5f));
            const Geometry *inputs[] = {&tmpl};
            return fx->cook(std::span<const Geometry *const>{inputs, 1});
        };

        Geometry a = run(7);
        Geometry b = run(7);
        Geometry c = run(8);

        bool same_seed_equal = true, diff_seed_differs = false, moved = false;
        for (size_t i = 0; i < 8; ++i)
        {
            const Vec3 &pa = a.positions()[i];
            const Vec3 &pb = b.positions()[i];
            const Vec3 &pc = c.positions()[i];
            if (!(pa.x == pb.x && pa.y == pb.y && pa.z == pb.z)) same_seed_equal = false;
            if (pa.x != pc.x || pa.y != pc.y || pa.z != pc.z) diff_seed_differs = true;
            if (!approxEq(pa.y, 0.0f)) moved = true;
        }
        check(same_seed_equal, "random_effector: same seed ⇒ byte-equal output");
        check(diff_seed_differs, "random_effector: different seed ⇒ different output");
        check(moved, "random_effector: position_amount actually displaces points");
    }

    // ───────────────────────────────────────────────────────────────────
    // 8) Per-clone `orient` quaternion → copy_to_points (CPU path).
    //    orient wins over the N-derived frame.
    // ───────────────────────────────────────────────────────────────────
    {
        Geometry stamp = GeometryConverter::fromSceneObject(SceneObject::createCube(0.5f));

        // 45° about Z, wxyz = (cos 22.5°, 0, 0, sin 22.5°).
        const float c = 0.92387953f, s = 0.38268343f;
        const Vec4 orient45(c, 0.0f, 0.0f, s);

        Geometry tmpl;
        auto *P  = tmpl.points().add<Vec3>("P", Vec3(0.0f));
        // N points +X: if N drove the frame, +Z would map to +X. orient must win.
        auto *N  = tmpl.points().add<Vec3>("N", Vec3(1.0f, 0.0f, 0.0f));
        auto *O  = tmpl.points().add<Vec4>("orient", Vec4(1, 0, 0, 0));
        tmpl.resizePoints(1);
        P->data()[0] = Vec3(0.0f);
        N->data()[0] = Vec3(1.0f, 0.0f, 0.0f);
        O->data()[0] = orient45;

        auto copy = SopRegistry::instance().create("copy_to_points", 0);
        const Geometry *inputs[] = {&stamp, &tmpl};
        Geometry out = copy->cook(std::span<const Geometry *const>{inputs, 2});

        // Corner (0.25, 0.25, 0.25) rotated 45° about Z → (0, 0.353553, 0.25).
        const auto &op = out.positions();
        bool found_rotated = false, found_unrotated = false;
        for (const auto &p : op)
        {
            if (approxEq(p.x, 0.0f) && approxEq(p.y, 0.35355339f) && approxEq(p.z, 0.25f))
                found_rotated = true;
            if (approxEq(p.x, 0.25f) && approxEq(p.y, 0.25f) && approxEq(p.z, 0.25f))
                found_unrotated = true;
        }
        check(found_rotated,   "orient→copy_to_points: corner landed Z-rotated 45°");
        check(!found_unrotated, "orient→copy_to_points: unrotated corner absent");

        // Identity orient + N=+X: orient (even identity) wins over N.
        O->data()[0] = Vec4(1, 0, 0, 0);
        Geometry out2 = copy->cook(std::span<const Geometry *const>{inputs, 2});
        bool identity_kept = false;
        for (const auto &p : out2.positions())
        {
            if (approxEq(p.x, 0.25f) && approxEq(p.y, 0.25f) && approxEq(p.z, 0.25f))
                identity_kept = true;
        }
        check(identity_kept, "orient precedence: identity orient beats N frame");
    }

    // ───────────────────────────────────────────────────────────────────
    // 9) Per-clone `orient` → instance emit (EmittedActor.InstanceEntry).
    //    Uses a test-local generator SOP that authors the orient attr.
    // ───────────────────────────────────────────────────────────────────
    {
        // Registered once, in this test binary only.
        class OrientPointSop : public SopNode
        {
        public:
            explicit OrientPointSop(size_t uid) : SopNode(uid) {}
            std::string kind() const override { return "test_orient_point"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                return io;
            }
            Geometry cook(std::span<const Geometry *const>) const override
            {
                Geometry g;
                auto *P = g.points().add<Vec3>("P", Vec3(0.0f));
                auto *N = g.points().add<Vec3>("N", Vec3(1.0f, 0.0f, 0.0f));
                auto *O = g.points().add<Vec4>("orient", Vec4(1, 0, 0, 0));
                g.resizePoints(1);
                P->data()[0] = Vec3(1.0f, 2.0f, 3.0f);
                N->data()[0] = Vec3(1.0f, 0.0f, 0.0f);
                O->data()[0] = Vec4(0.92387953f, 0.0f, 0.0f, 0.38268343f);  // 45° Z
                return g;
            }
        };
        SopRegistry::instance().registerType(
            {"test_orient_point", "Test Orient Point", "Generators", {}, {{"out"}}, {}},
            [](size_t uid) -> std::unique_ptr<SopNode> {
                return std::make_unique<OrientPointSop>(uid);
            });

        SopGraph g(0);
        auto cube = SopRegistry::instance().create("primitive_cube", g.nextUid());
        const size_t cubeUid = cube->uid();
        g.addNode(std::move(cube));
        auto tpl = SopRegistry::instance().create("test_orient_point", g.nextUid());
        const size_t tplUid = tpl->uid();
        g.addNode(std::move(tpl));
        auto inst = SopRegistry::instance().create("instance", g.nextUid());
        const size_t instUid = inst->uid();
        g.addNode(std::move(inst));

        g.createConnection(cubeUid, 0, instUid, 0);  // stamp
        g.createConnection(tplUid, 0, instUid, 1);   // template

        CookDiagnostic diag;
        auto emitted = g.cook(&diag);
        check(diag.ok, "orient→instance: cook ok");
        bool entry_ok = false;
        for (const auto &ea : emitted)
        {
            if (ea.instances.size() != 1) continue;
            const auto &e = ea.instances[0];
            if (approxEq(e.rotation.x, 0.92387953f) && approxEq(e.rotation.y, 0.0f) &&
                approxEq(e.rotation.z, 0.0f) && approxEq(e.rotation.w, 0.38268343f) &&
                approxEq(e.translate.x, 1.0f) && approxEq(e.translate.y, 2.0f) &&
                approxEq(e.translate.z, 3.0f))
            {
                entry_ok = true;
            }
        }
        check(entry_ok, "orient→instance: InstanceEntry.rotation carries the 45° Z quat");
    }

    // ───────────────────────────────────────────────────────────────────
    // 10) noise_effector field stability + step_effector index ramp.
    // ───────────────────────────────────────────────────────────────────
    {
        Geometry tmpl;
        auto *P = tmpl.points().add<Vec3>("P", Vec3(0.0f));
        tmpl.resizePoints(6);
        for (size_t i = 0; i < 6; ++i)
            P->data()[i] = Vec3(static_cast<float>(i) * 0.7f, 0.3f, 0.1f);

        auto runNoise = [&tmpl](float offX) {
            auto fx = SopRegistry::instance().create("noise_effector", 0);
            fx->setParamVec3("position_amount", Vec3(0.0f, 1.0f, 0.0f));
            fx->setParamFloat("frequency", 1.3f);
            fx->setParamVec3("offset", Vec3(offX, 0.0f, 0.0f));
            const Geometry *inputs[] = {&tmpl};
            return fx->cook(std::span<const Geometry *const>{inputs, 1});
        };

        Geometry n0 = runNoise(0.0f);
        Geometry n0b = runNoise(0.0f);
        Geometry n10 = runNoise(10.0f);

        bool reproducible = true, offset_changes = false;
        for (size_t i = 0; i < 6; ++i)
        {
            if (n0.positions()[i].y != n0b.positions()[i].y) reproducible = false;
            if (!approxEq(n0.positions()[i].y, n10.positions()[i].y)) offset_changes = true;
        }
        check(reproducible, "noise_effector: same offset ⇒ identical field");
        check(offset_changes, "noise_effector: shifting offset moves the field");

        auto step = SopRegistry::instance().create("step_effector", 0);
        step->setParamFloat("scale", 3.0f);
        const Geometry *inputs[] = {&tmpl};
        Geometry st = step->cook(std::span<const Geometry *const>{inputs, 1});
        const auto *ps = st.points().get<float>("pscale");
        check(ps != nullptr, "step_effector: pscale materialised");
        if (ps)
        {
            bool monotone = true;
            for (size_t i = 1; i < 6; ++i)
                if (ps->data()[i] < ps->data()[i - 1]) monotone = false;
            check(monotone, "step_effector: pscale non-decreasing with index");
            check(approxEq(ps->data()[0], 1.0f) && approxEq(ps->data()[5], 3.0f),
                  "step_effector: ramp endpoints (1 → scale)");

            step->setParamBool("reverse", true);
            Geometry str = step->cook(std::span<const Geometry *const>{inputs, 1});
            const auto *psr = str.points().get<float>("pscale");
            check(psr && approxEq(psr->data()[0], 3.0f) && approxEq(psr->data()[5], 1.0f),
                  "step_effector: reverse flips the ramp");
        }
    }

    // ───────────────────────────────────────────────────────────────────
    // 11) Generalized param animation: a keyed param on an ORDINARY node
    //     (transform) makes it time-dependent and its cook samples the
    //     channel at the playhead — no per-node time plumbing.
    // ───────────────────────────────────────────────────────────────────
    {
        Geometry cube = GeometryConverter::fromSceneObject(SceneObject::createCube(0.5f));

        auto xf = SopRegistry::instance().create("transform", 0);
        check(!xf->isTimeDependent(), "unkeyed transform is not time-dependent");

        // Key translate.x: 0 at t=0s → 5 at t=1s (linear).
        for (auto &p : xf->parameters())
        {
            if (p.name != "translate") continue;
            p.channels.resize(3);
            tracey::sops::ScalarChannel::Key k0;
            k0.time = 0.0; k0.value = 0.0f;
            tracey::sops::ScalarChannel::Key k1;
            k1.time = 1.0; k1.value = 5.0f;
            p.channels[0].setKey(k0);
            p.channels[0].setKey(k1);
        }
        check(xf->isTimeDependent(), "keyed translate.x makes transform time-dependent");

        const Geometry *inputs[] = {&cube};
        Geometry at0 = xf->cookAt(std::span<const Geometry *const>{inputs, 1}, 0.0);
        Geometry at1 = xf->cookAt(std::span<const Geometry *const>{inputs, 1}, 1.0);

        // The keyed channel moves the whole cube +5 in X at t=1.
        float minX0 = 1e9f, minX1 = 1e9f;
        for (const auto &p : at0.positions()) minX0 = std::min(minX0, p.x);
        for (const auto &p : at1.positions()) minX1 = std::min(minX1, p.x);
        check(approxEq(minX0, -0.25f), "keyed transform: t=0 samples key 0");
        check(approxEq(minX1, 4.75f), "keyed transform: t=1 samples key 1 (no cookAt override needed)");

        // The plain effector animates its strength the same way.
        Geometry tmpl;
        auto *P = tmpl.points().add<Vec3>("P", Vec3(0.0f));
        tmpl.resizePoints(1);
        P->data()[0] = Vec3(0.0f);

        auto fx = SopRegistry::instance().create("plain_effector", 0);
        fx->setParamVec3("position", Vec3(0.0f, 2.0f, 0.0f));
        for (auto &p : fx->parameters())
        {
            if (p.name != "strength") continue;
            p.channels.resize(1);
            tracey::sops::ScalarChannel::Key k0;
            k0.time = 0.0; k0.value = 0.0f;
            tracey::sops::ScalarChannel::Key k1;
            k1.time = 1.0; k1.value = 1.0f;
            p.channels[0].setKey(k0);
            p.channels[0].setKey(k1);
        }
        check(fx->isTimeDependent(), "keyed strength makes effector time-dependent");
        const Geometry *einputs[] = {&tmpl};
        Geometry e0 = fx->cookAt(std::span<const Geometry *const>{einputs, 1}, 0.0);
        Geometry e1 = fx->cookAt(std::span<const Geometry *const>{einputs, 1}, 1.0);
        check(approxEq(e0.positions()[0].y, 0.0f) && approxEq(e1.positions()[0].y, 2.0f),
              "keyed effector strength: 0 at t=0, full offset at t=1");
    }

    if (failures == 0) std::printf("[cloners_smoke] all checks passed\n");
    else               std::printf("[cloners_smoke] %d failure(s)\n", failures);
    return failures > 0 ? 1 : 0;
}
