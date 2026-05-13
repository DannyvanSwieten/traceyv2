// End-to-end smoke test for the DOP graph layer.
//
// Builds:  pop_source → pop_gravity → pop_solver
// Cooks frames 1..30 at 24 fps, asserts:
//   • Particle count grows during the emit window and stops growing once
//     the oldest particles begin to die (rate * lifetime = steady state).
//   • Particles fall under gravity (mean Y at frame N+1 < mean Y at N
//     once velocity has accumulated past the initial upward kick).
//   • Particle ids are unique (per the __pop_next_id counter).
//   • The DopGraph round-trips through serialize → deserialize byte-stable.
//   • clearCache() actually wipes; cookToFrame after a wipe restarts from
//     frame 0 and reproduces the same point count.
//
// Exit 0 on success, non-zero on first failed check. Depends only on
// `tracey`. Run with:
//   cmake --build build --target dop_smoke && ./build/examples/dop_smoke

#include "geometry/geometry.hpp"
#include "geometry/attribute_table.hpp"
#include "geometry/attribute.hpp"

#include "dops/dop_graph.hpp"
#include "dops/dop_node.hpp"
#include "dops/dop_registry.hpp"
#include "dops/register_builtins.hpp"
#include "dops/serialization.hpp"
#include "dops/sim_state.hpp"
#include "dops/nodes/pop_force.hpp"

#include "vops/register_builtins.hpp"
#include "vops/vop_graph.hpp"
#include "vops/vop_node.hpp"
#include "vops/vop_registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (ok) std::printf("  ok   %s\n", what);
    else { ++failures; std::printf("  FAIL %s\n", what); }
}

}

int main()
{
    using namespace tracey;
    using namespace tracey::dops;

    DopRegistry::instance();
    registerBuiltinDops();
    // pop_force needs the VOP registry — its embedded subnet contains
    // VOP nodes that get deserialized through the VOP registry.
    tracey::vops::registerBuiltinVops();

    // ── Build a particle DOP graph ─────────────────────────────────────
    // pop_source emits at rate=24/s with lifetime=1s. At 24fps with
    // dt=1/24s that's 1 particle per substep — predictable counts.
    auto graph = std::make_unique<DopGraph>(/*uid=*/0);

    auto src     = DopRegistry::instance().create("pop_source",  graph->nextUid());
    auto gravity = DopRegistry::instance().create("pop_gravity", graph->nextUid());
    auto solver  = DopRegistry::instance().create("pop_solver",  graph->nextUid());
    if (!src || !gravity || !solver) { std::fprintf(stderr, "factory failed\n"); return 2; }

    // Configure source: 24 particles/sec, 1.0s life, +Y initial v so we
    // can clearly see gravity bend them back down.
    src->setParamFloat("rate", 24.0f);
    src->setParamFloat("lifetime", 1.0f);
    src->setParamVec3 ("initial_v", Vec3(0.0f, 5.0f, 0.0f));
    src->setParamFloat("pos_jitter", 0.0f);
    src->setParamInt  ("seed", 42);

    // Stronger gravity so the position curve flips within the 1s life.
    gravity->setParamVec3("gravity", Vec3(0.0f, -9.81f, 0.0f));

    // Wire source → gravity → solver so topo sort orders them right.
    const size_t srcUid     = src->uid();
    const size_t gravityUid = gravity->uid();
    const size_t solverUid  = solver->uid();
    graph->addNode(std::move(src));
    graph->addNode(std::move(gravity));
    graph->addNode(std::move(solver));
    graph->createConnection(srcUid,     0, gravityUid, 0);
    graph->createConnection(gravityUid, 0, solverUid,  0);
    graph->markDirty();

    // ── Cook frames 1..30 at 24 fps ────────────────────────────────────
    const double fps = 24.0;
    const int    targetFrame = 30;
    graph->cookToFrame(targetFrame, fps);

    check(graph->cachedToFrame() == targetFrame, "cookToFrame populates the cache");

    // Sanity: every frame's geometry should have P, v, age, life, id, force.
    {
        const SimState *s = graph->frame(targetFrame);
        check(s != nullptr, "frame() returns the cached SimState");
        const auto &pts = s->geometry.points();
        check(pts.has("P")      , "P attribute present");
        check(pts.has("v")      , "v attribute present");
        check(pts.has("age")    , "age attribute present");
        check(pts.has("life")   , "life attribute present");
        check(pts.has("id")     , "id attribute present");
        check(pts.has("force")  , "force attribute present");
    }

    // Population dynamics. At rate=24/s with lifetime=1.0s and dt=1/24s
    // the steady-state count is ~24 once kills catch emissions. Walk the
    // counts and assert growth phase + steady-state plateau.
    int peakCount = 0;
    int peakFrame = 0;
    for (int f = 1; f <= targetFrame; ++f)
    {
        const SimState *s = graph->frame(f);
        if (!s) continue;
        const int n = static_cast<int>(s->geometry.pointCount());
        if (n > peakCount) { peakCount = n; peakFrame = f; }
    }
    check(peakCount >= 20 && peakCount <= 30,
          "peak count is near steady-state (rate * lifetime ≈ 24)");
    check(peakFrame >= 20, "peak count is reached after lifetime ramp-up");

    // Gravity check. Comparing position Y across frames is misleading —
    // in steady state, particles are distributed across the full
    // trajectory and the mean Y converges to ∫y(t)dt, which can exceed
    // the early-fill state. Velocity is the clean signal: mean v_y
    // monotonically drops as gravity pulls every live particle down.
    // At frame 5, mean v_y ≈ 5 - g*(2/24) ≈ 4.18; at frame 25 steady
    // state, mean v_y ≈ 5 - g/2 ≈ 0.10.
    auto meanVy = [&](int frameIdx) -> double {
        const SimState *s = graph->frame(frameIdx);
        if (!s || s->geometry.pointCount() == 0) return 0.0;
        const auto *V = s->geometry.points().get<Vec3>("v");
        if (!V) return 0.0;
        double sum = 0.0;
        for (const auto &v : V->data()) sum += v.y;
        return sum / static_cast<double>(V->data().size());
    };
    const double earlyVy = meanVy(5);
    const double lateVy  = meanVy(25);
    check(lateVy < earlyVy, "gravity decelerated particles (lateVy < earlyVy)");

    // Unique ids. We never re-use ids after a kill, so ids in the live
    // set are a subset of {0, 1, ..., total_emitted-1} with no dupes.
    {
        const SimState *s = graph->frame(targetFrame);
        const auto *idAttr = s ? s->geometry.points().get<int>("id") : nullptr;
        check(idAttr != nullptr, "id attribute is typed int");
        if (idAttr)
        {
            const auto &ids = idAttr->data();
            std::set<int> uniq(ids.begin(), ids.end());
            check(uniq.size() == ids.size(), "particle ids are unique");
        }
    }

    // Serialization round-trip — graph topology + params survive.
    {
        const std::string txt = serializeDopGraph(*graph);
        auto reloaded = deserializeDopGraph(txt);
        check(reloaded != nullptr, "deserialize returns non-null");
        if (reloaded)
        {
            check(reloaded->nodes().size() == 3, "round-trip: 3 nodes");
            check(reloaded->connections().size() == 2, "round-trip: 2 connections");
            const std::string txt2 = serializeDopGraph(*reloaded);
            check(txt == txt2, "round-trip: byte-stable");
        }
    }

    // Cache invalidation + re-cook: the same graph cooked from scratch
    // should produce the same point count at the target frame.
    {
        const int countBefore = static_cast<int>(graph->frame(targetFrame)->geometry.pointCount());
        graph->clearCache();
        check(graph->cachedToFrame() == 0, "clearCache resets to frame 0");
        graph->cookToFrame(targetFrame, fps);
        const int countAfter = static_cast<int>(graph->frame(targetFrame)->geometry.pointCount());
        check(countBefore == countAfter, "re-cook from scratch is deterministic");
    }

    // ── pop_force with a VOP subnet that produces a constant +X force ──
    // Verifies Phase 4 end-to-end: build a graph with pop_source →
    // pop_force → pop_solver; pop_force's subnet is a constant_vec3
    // wired into geo_output.force. Particles should drift in +X.
    {
        auto fgraph = std::make_unique<DopGraph>(0);

        auto src    = DopRegistry::instance().create("pop_source", fgraph->nextUid());
        auto force  = DopRegistry::instance().create("pop_force",  fgraph->nextUid());
        auto solver = DopRegistry::instance().create("pop_solver", fgraph->nextUid());

        src->setParamFloat("rate", 24.0f);
        src->setParamFloat("lifetime", 2.0f);
        // Zero initial velocity — any motion in +X must come from pop_force.
        src->setParamVec3 ("initial_v", Vec3(0.0f));

        const size_t srcUid   = src->uid();
        const size_t forceUid = force->uid();
        const size_t solverUid = solver->uid();
        fgraph->addNode(std::move(src));
        fgraph->addNode(std::move(force));
        fgraph->addNode(std::move(solver));
        fgraph->createConnection(srcUid,   0, forceUid,  0);
        fgraph->createConnection(forceUid, 0, solverUid, 0);

        // Replace pop_force's seeded subnet with a constant_vec3 →
        // geo_output.force wiring producing (5, 0, 0) m/s². Port index
        // 5 on geo_output is `force` (see kVecPorts in geo_io_vops.cpp).
        auto* forceNode = fgraph->findNode(forceUid);
        check(forceNode != nullptr, "pop_force: node lookup");
        auto subnet = std::make_unique<tracey::vops::VopGraph>(0);
        auto& vreg = tracey::vops::VopRegistry::instance();
        auto cv  = vreg.create("constant_vec3", subnet->nextUid());
        auto go  = vreg.create("geo_output",    subnet->nextUid());
        check(cv != nullptr && go != nullptr, "pop_force: VOP factories present");
        if (cv && go)
        {
            cv->setParamVec3("value", Vec3(5.0f, 0.0f, 0.0f));
            const size_t cvUid = cv->uid();
            const size_t goUid = go->uid();
            subnet->addNode(std::move(cv));
            subnet->addNode(std::move(go));
            constexpr size_t kForcePort = 5;
            subnet->createConnection(cvUid, 0, goUid, kForcePort);
        }
        tracey::dops::setPopForceVopGraph(forceNode, std::move(subnet));
        fgraph->markDirty();

        fgraph->cookToFrame(targetFrame, fps);
        const SimState* st = fgraph->frame(targetFrame);
        check(st != nullptr && st->geometry.pointCount() > 0,
              "pop_force: cook produced particles");
        if (st && st->geometry.pointCount() > 0)
        {
            const auto* V = st->geometry.points().get<Vec3>("v");
            check(V != nullptr, "pop_force: v attribute exists");
            // Mean v_x > 0 confirms the VOP subnet's constant force
            // propagated through geo_output.force → pop_solver's integrator.
            double sumVx = 0.0;
            for (const auto& v : V->data()) sumVx += v.x;
            const double meanVx = sumVx / static_cast<double>(V->data().size());
            check(meanVx > 0.5,
                  "pop_force: VOP subnet drove particles in +X (meanVx > 0.5)");
        }

        // Round-trip the pop_force graph including the embedded VOP.
        const std::string txt = serializeDopGraph(*fgraph);
        auto reloaded = deserializeDopGraph(txt);
        check(reloaded != nullptr, "pop_force: deserialize");
        if (reloaded)
        {
            const auto* dupForce = reloaded->findNode(forceUid);
            const auto* dupVop = tracey::dops::popForceVopGraph(dupForce);
            check(dupVop != nullptr && dupVop->nodes().size() == 2,
                  "pop_force: embedded VOP graph round-trips with 2 nodes");
        }
    }

    std::printf("dop_smoke: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
