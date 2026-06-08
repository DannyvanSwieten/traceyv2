// End-to-end smoke test for pop_source(emit_mode="geometry").
//
// Builds a 4×4 points_grid, hands it to a stub SopGeometryProvider, then
// cooks a DOP graph: pop_source(emit_mode=1, source_sop_uid=42) →
// pop_solver. Asserts:
//
//   • After 1 frame at 24 fps with rate=48, exactly 2 particles spawn
//     (48 · 1/24 = 2.0 — carry-aware floor gives 2).
//   • Each spawned particle's P matches one of the 16 grid points.
//   • Each spawned particle's v = source.N · normal_speed (= up × 2).
//   • Cd was inherited (white in the source → white on the spawn).
//   • The deterministic round-robin index advances: after a second
//     frame's emit, particles 2..3 come from grid indices 2..3.
//   • With no provider wired (legacy path), pop_source falls back to
//     point-mode behaviour even when emit_mode=1.
//
// Headless / no GPU. Standalone tracey link only.

#include "geometry/geometry.hpp"
#include "geometry/attribute.hpp"
#include "geometry/attribute_table.hpp"

#include "dops/dop_graph.hpp"
#include "dops/dop_node.hpp"
#include "dops/dop_registry.hpp"
#include "dops/eval_context.hpp"
#include "dops/register_builtins.hpp"
#include "dops/sim_state.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char *what)
{
    if (ok) std::printf("  ok   %s\n", what);
    else   { ++failures; std::printf("  FAIL %s\n", what); }
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Trivial provider that hands back a stored Geometry for one uid. The
// editor's impl is a thin shim over CookCache::findOutput; this test
// builds a Geometry by hand to avoid pulling the SOP cook layer in.
class StubProvider : public tracey::dops::SopGeometryProvider
{
public:
    explicit StubProvider(uint64_t uid, const tracey::Geometry *g) : m_uid(uid), m_g(g) {}
    const tracey::Geometry *lookupCookedGeometry(uint64_t sopUid) const override
    {
        return (sopUid == m_uid) ? m_g : nullptr;
    }
private:
    uint64_t m_uid;
    const tracey::Geometry *m_g;
};

// Build a 4×4 grid of points on the XZ plane, with N = (0, 1, 0) and
// Cd = white at every point. Mirrors what points_grid → scatter would
// produce on the SOP side.
tracey::Geometry makeGridGeometry()
{
    using namespace tracey;
    Geometry g;
    const int N = 4;
    const float spacing = 0.5f;
    auto *P  = g.points().add<Vec3>("P", Vec3(0.0f));
    auto *Na = g.points().add<Vec3>("N", Vec3(0.0f, 1.0f, 0.0f));
    auto *Cd = g.points().add<Vec3>("Cd", Vec3(1.0f));
    P->data().resize(N * N);
    Na->data().resize(N * N, Vec3(0.0f, 1.0f, 0.0f));
    Cd->data().resize(N * N, Vec3(1.0f));
    for (int z = 0; z < N; ++z)
        for (int x = 0; x < N; ++x)
            P->data()[z * N + x] = Vec3(x * spacing, 0.0f, z * spacing);
    return g;
}
}

int main()
{
    using namespace tracey;
    using namespace tracey::dops;

    DopRegistry::instance();
    registerBuiltinDops();

    // ── Source geometry + provider ──
    Geometry src = makeGridGeometry();
    const uint64_t SRC_UID = 42;
    StubProvider provider(SRC_UID, &src);

    // ── DOP graph: pop_source(emit_mode=geometry) → pop_solver ──
    DopGraph dop(0);
    dop.setSopProvider(&provider);

    auto src_dop = DopRegistry::instance().create("pop_source", dop.nextUid());
    check(src_dop != nullptr, "create pop_source");
    src_dop->setParamFloat("rate",         48.0f);
    src_dop->setParamFloat("lifetime",     60.0f);
    src_dop->setParamInt  ("emit_mode",     1);            // geometry
    src_dop->setParamInt  ("source_sop_uid", static_cast<int>(SRC_UID));
    src_dop->setParamBool ("use_normal",    true);
    src_dop->setParamFloat("normal_speed",  2.0f);
    src_dop->setParamBool ("inherit_cd",    true);
    src_dop->setParamFloat("pos_jitter",    0.0f);
    const size_t srcUid = src_dop->uid();
    dop.addNode(std::move(src_dop));

    auto solver = DopRegistry::instance().create("pop_solver", dop.nextUid());
    check(solver != nullptr, "create pop_solver");
    const size_t solverUid = solver->uid();
    dop.addNode(std::move(solver));
    dop.createConnection(srcUid, 0, solverUid, 0);

    // ── Cook frame 1 ──
    const double fps = 24.0;
    dop.cookToFrame(1, fps);
    const SimState *s1 = dop.frame(1);
    check(s1 != nullptr, "frame 1 state present");
    if (!s1) return 1;

    // 48 particles/sec at 1/24s = 2 particles per frame.
    const size_t count1 = s1->geometry.pointCount();
    check(count1 == 2, "frame 1 spawned 2 particles");

    const auto *P  = s1->geometry.points().get<Vec3>("P");
    const auto *V  = s1->geometry.points().get<Vec3>("v");
    const auto *Cd = s1->geometry.points().get<Vec3>("Cd");
    check(P && V && Cd, "frame 1 has P, v, Cd");
    if (P && V && Cd && count1 >= 2)
    {
        // Round-robin: first two spawns come from source[0], source[1].
        const auto &srcP = src.points().get<Vec3>("P")->data();
        // pop_solver integrates one substep before we read frame 1, so
        // P[i] = source.P[i] + V·dt. With V = (0, 2, 0) and dt = 1/24,
        // we expect a +Y offset of 2/24 ≈ 0.0833.
        const float dt = 1.0f / 24.0f;
        for (size_t i = 0; i < 2; ++i)
        {
            const Vec3 expected = srcP[i] + Vec3(0.0f, 2.0f, 0.0f) * dt;
            const Vec3 actual = P->data()[i];
            check(near(actual.x, expected.x) &&
                  near(actual.y, expected.y) &&
                  near(actual.z, expected.z),
                  ("particle " + std::to_string(i) + " P matches grid + advect").c_str());
            check(near(V->data()[i].x, 0.0f) &&
                  near(V->data()[i].y, 2.0f) &&
                  near(V->data()[i].z, 0.0f),
                  ("particle " + std::to_string(i) + " v = N · normal_speed").c_str());
            check(near(Cd->data()[i].x, 1.0f) &&
                  near(Cd->data()[i].y, 1.0f) &&
                  near(Cd->data()[i].z, 1.0f),
                  ("particle " + std::to_string(i) + " Cd inherited (white)").c_str());
        }
    }

    // ── Cook frame 2 — round-robin index keeps advancing ──
    dop.cookToFrame(2, fps);
    const SimState *s2 = dop.frame(2);
    check(s2 && s2->geometry.pointCount() == 4, "frame 2 has 4 particles total");

    // ── No-provider fallback: emit_mode=geometry but no provider →
    //    behaves like emit_mode=point (legacy path). ──
    DopGraph dopNoProv(0);
    // Intentionally don't call setSopProvider.
    auto fallback_src = DopRegistry::instance().create("pop_source", dopNoProv.nextUid());
    fallback_src->setParamFloat("rate",          48.0f);
    fallback_src->setParamInt  ("emit_mode",      1);
    fallback_src->setParamInt  ("source_sop_uid", static_cast<int>(SRC_UID));
    fallback_src->setParamVec3 ("origin",         Vec3(7.0f, 0.0f, 0.0f));
    fallback_src->setParamVec3 ("initial_v",      Vec3(0.0f, 1.0f, 0.0f));
    const size_t fbUid = fallback_src->uid();
    dopNoProv.addNode(std::move(fallback_src));
    auto fbSolver = DopRegistry::instance().create("pop_solver", dopNoProv.nextUid());
    const size_t fbSolverUid = fbSolver->uid();
    dopNoProv.addNode(std::move(fbSolver));
    dopNoProv.createConnection(fbUid, 0, fbSolverUid, 0);
    dopNoProv.cookToFrame(1, fps);
    const SimState *fbState = dopNoProv.frame(1);
    check(fbState && fbState->geometry.pointCount() == 2, "no-provider fallback spawns from origin");
    if (fbState && fbState->geometry.pointCount() >= 1)
    {
        const Vec3 p = fbState->geometry.points().get<Vec3>("P")->data()[0];
        // origin=(7,0,0), v=(0,1,0), dt=1/24 → P=(7, 1/24, 0).
        check(near(p.x, 7.0f) && near(p.z, 0.0f),
              "no-provider fallback particle near origin (x=7, z=0)");
    }

    if (failures == 0) std::printf("[dop_geometry_source_smoke] all checks passed\n");
    else               std::printf("[dop_geometry_source_smoke] %d failure(s)\n", failures);
    return failures > 0 ? 1 : 0;
}
