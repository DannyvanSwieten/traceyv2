// Self-contained smoke test for the SOP graph cook pipeline.
//
// Builds:  Cube → Transform → ObjectOutput
// Cooks the graph, asserts that:
//   • Exactly one EmittedActor came back.
//   • The emitted geometry has the expected vertex / triangle counts (a
//     non-indexed cube has 36 verts / 12 tris from the existing
//     SceneObject::createCube generator).
//   • The Transform SOP's translate parameter actually shifted positions.
//   • The Geometry → SceneObject conversion preserves vertex count.
//   • Catalog query exposes all v1 built-in node kinds.
//
// Exit 0 on success, non-zero on first failed check (with a printed message).
//
// This binary depends only on the `tracey` static lib. No Vulkan, no
// rendering pipeline — it exists to validate the SOP infrastructure in
// isolation. Wire it into CI / run by hand:
//   cmake --build build --target sop_eval_test && ./build/examples/sop_eval_test

#include "geometry/geometry.hpp"
#include "geometry/geometry_converter.hpp"
#include "scene/scene_object.hpp"

#include "sops/serialization.hpp"
#include "sops/sop_graph.hpp"
#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (ok)
    {
        std::printf("  ok   %s\n", what);
        return;
    }
    ++failures;
    std::printf("  FAIL %s\n", what);
}

template <typename T>
void check_eq(T got, T want, const char *what)
{
    if (got == want)
    {
        std::printf("  ok   %s\n", what);
        return;
    }
    ++failures;
    std::printf("  FAIL %s  (got %lld, want %lld)\n", what,
                static_cast<long long>(got), static_cast<long long>(want));
}

} // anon

int main()
{
    using namespace tracey;
    using namespace tracey::sops;

    std::printf("[sop_eval_test] registering built-ins\n");
    registerBuiltinSops();

    std::printf("[sop_eval_test] catalog has %zu entries\n",
                SopRegistry::instance().catalog().size());
    check(SopRegistry::instance().has("primitive_cube"),    "catalog: primitive_cube");
    check(SopRegistry::instance().has("primitive_sphere"),  "catalog: primitive_sphere");
    check(SopRegistry::instance().has("primitive_plane"),   "catalog: primitive_plane");
    check(SopRegistry::instance().has("primitive_torus"),   "catalog: primitive_torus");
    check(SopRegistry::instance().has("primitive_cylinder"),"catalog: primitive_cylinder");
    check(SopRegistry::instance().has("primitive_cone"),    "catalog: primitive_cone");
    check(SopRegistry::instance().has("transform"),         "catalog: transform");
    check(SopRegistry::instance().has("merge"),             "catalog: merge");
    check(SopRegistry::instance().has("gltf_import"),       "catalog: gltf_import");
    check(SopRegistry::instance().has("object_output"),     "catalog: object_output");

    // ── Build a small graph: Cube → Transform → ObjectOutput ──────────────
    std::printf("[sop_eval_test] building Cube → Transform → ObjectOutput\n");
    SopGraph graph(0);

    auto cube = SopRegistry::instance().create("primitive_cube", graph.nextUid());
    auto xform = SopRegistry::instance().create("transform",     graph.nextUid());
    auto out   = SopRegistry::instance().create("object_output", graph.nextUid());
    check(cube && xform && out, "graph construction: nodes created");

    // Translate +5 on X.
    xform->setParamVec3("translate", {5.0f, 0.0f, 0.0f});

    const size_t cubeUid = cube->uid();
    const size_t xformUid = xform->uid();
    const size_t outUid = out->uid();

    graph.addNode(std::move(cube));
    graph.addNode(std::move(xform));
    graph.addNode(std::move(out));
    graph.createConnection(cubeUid, 0, xformUid, 0);
    graph.createConnection(xformUid, 0, outUid, 0);

    // ── Cook ──────────────────────────────────────────────────────────────
    CookDiagnostic diag;
    auto emitted = graph.cook(&diag);
    if (!diag.ok)
    {
        std::printf("  FAIL cook: %s (node uid=%zu)\n", diag.message.c_str(), diag.nodeUid);
        ++failures;
    }
    check_eq<size_t>(emitted.size(), 1, "cook: emitted exactly one actor");

    if (!emitted.empty())
    {
        const auto &ea = emitted[0];
        // 36 corners (6 faces * 2 tris * 3 verts) — matches SceneObject::createCube.
        check_eq<size_t>(ea.geometry.pointCount(),    36, "cube: pointCount");
        check_eq<size_t>(ea.geometry.vertexCount(),   36, "cube: vertexCount");
        check_eq<size_t>(ea.geometry.primitiveCount(), 12, "cube: primitiveCount (12 tris)");

        // Translate of +5 X should shift every position.
        const auto &positions = ea.geometry.positions();
        bool shifted = !positions.empty();
        for (const auto &p : positions)
        {
            // Cube was centred at origin (-0.5, -0.5, -0.5)..(0.5, 0.5, 0.5).
            // After +5 X, every X coord must be in [4.5, 5.5].
            if (p.x < 4.49f || p.x > 5.51f) { shifted = false; break; }
        }
        check(shifted, "transform: positions shifted by translate(+5,0,0)");

        // ── SceneObject conversion ────────────────────────────────────────
        SceneObject so = GeometryConverter::toSceneObject(ea.geometry, "cube_test");
        check_eq<size_t>(so.positions().size(), 36, "toSceneObject: vertex count");
        check_eq<size_t>(so.triangleCount(), 12,    "toSceneObject: triangle count");
    }

    // ── JSON round-trip ───────────────────────────────────────────────────
    std::printf("[sop_eval_test] JSON round-trip\n");
    const std::string jsonText = serializeSopGraph(graph);
    check(!jsonText.empty(), "serialize produced non-empty json");

    auto reloaded = deserializeSopGraph(jsonText);
    check(reloaded != nullptr, "deserialize succeeded");
    if (reloaded)
    {
        check_eq<size_t>(reloaded->nodes().size(),       graph.nodes().size(),       "round-trip: node count");
        check_eq<size_t>(reloaded->connections().size(), graph.connections().size(), "round-trip: connection count");

        auto reloadEmit = reloaded->cook();
        check_eq<size_t>(reloadEmit.size(), 1, "round-trip: cook still emits one actor");
        if (!reloadEmit.empty())
        {
            check_eq<size_t>(reloadEmit[0].geometry.pointCount(), 36,
                             "round-trip: cube geometry intact");
        }
    }

    if (failures > 0)
    {
        std::printf("\n[sop_eval_test] %d failure(s)\n", failures);
        return 1;
    }
    std::printf("\n[sop_eval_test] all checks passed\n");
    return 0;
}
