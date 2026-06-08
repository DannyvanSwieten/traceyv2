// End-to-end smoke test for the VOP graph layer.
//
// Builds the SOP graph:  cube → attribute_vop → object_output
// Populates the attribute_vop's child VopGraph with:
//   geo_input.P → noise_perlin → multiply(constant_float 0.1)
//               → add(geo_input.P) → geo_output.P
// Cooks the SOP graph, asserts:
//   • Exactly one EmittedActor came back.
//   • The emitted geometry has the cube's original triangle count.
//   • At least one position differs from the input cube's positions
//     (i.e. the noise was actually applied).
//   • The VopGraph round-trips through serialize → deserialize byte-stable.
//
// Exit 0 on success, non-zero on first failed check.
//
// Depends only on `tracey`. No Vulkan, no rendering. Run by hand:
//   cmake --build build --target attribute_vop_smoke && \
//     ./build/examples/attribute_vop_smoke

#include "geometry/geometry.hpp"
#include "scene/scene_object.hpp"

#include "sops/serialization.hpp"
#include "sops/sop_graph.hpp"
#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"
#include "sops/nodes/attribute_vop_sop.hpp"

#include "vops/register_builtins.hpp"
#include "vops/serialization.hpp"
#include "vops/vop_graph.hpp"
#include "vops/vop_node.hpp"
#include "vops/vop_registry.hpp"

#include <cstdio>
#include <cstdlib>
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
    using namespace tracey::sops;

    SopRegistry::instance(); // ensure singleton instantiated
    sops::registerBuiltinSops();
    vops::registerBuiltinVops();

    // ── Build the SOP graph: cube → attribute_vop → object_output ──
    SopGraph sopGraph(/*uid=*/0);

    auto cube = SopRegistry::instance().create("primitive_cube", sopGraph.nextUid());
    check(cube != nullptr, "create primitive_cube");
    cube->setParamFloat("size", 2.0f);
    const size_t cubeUid = cube->uid();
    sopGraph.addNode(std::move(cube));

    auto avop = SopRegistry::instance().create("attribute_vop", sopGraph.nextUid());
    check(avop != nullptr, "create attribute_vop");
    const size_t avopUid = avop->uid();
    auto *avopRaw = avop.get();
    sopGraph.addNode(std::move(avop));

    auto outNode = SopRegistry::instance().create("object_output", sopGraph.nextUid());
    check(outNode != nullptr, "create object_output");
    outNode->setParamString("name", "vop_cube");
    const size_t outUid = outNode->uid();
    sopGraph.addNode(std::move(outNode));

    sopGraph.createConnection(cubeUid, 0, avopUid, 0);
    sopGraph.createConnection(avopUid, 0, outUid, 0);

    // ── Populate the AttributeVopSop's child VopGraph ──
    vops::VopGraph *vop = attributeVopGraph(avopRaw);
    check(vop != nullptr, "host exposes VopGraph");
    if (!vop) { return failures > 0 ? 1 : 0; }

    auto geoIn  = vops::VopRegistry::instance().create("geo_input",  vop->nextUid());
    auto noise  = vops::VopRegistry::instance().create("noise_perlin", vop->nextUid());
    auto kf     = vops::VopRegistry::instance().create("constant_float", vop->nextUid());
    auto mul    = vops::VopRegistry::instance().create("multiply", vop->nextUid());
    auto add    = vops::VopRegistry::instance().create("add", vop->nextUid());
    auto geoOut = vops::VopRegistry::instance().create("geo_output", vop->nextUid());
    check(geoIn && noise && kf && mul && add && geoOut, "create all VOP nodes");

    noise->setParamFloat("frequency", 1.5f);
    noise->setParamFloat("amplitude", 1.0f);
    kf->setParamFloat("value", 0.2f);

    const size_t geoInUid  = geoIn->uid();
    const size_t noiseUid  = noise->uid();
    const size_t kfUid     = kf->uid();
    const size_t mulUid    = mul->uid();
    const size_t addUid    = add->uid();
    const size_t geoOutUid = geoOut->uid();

    vop->addNode(std::move(geoIn));
    vop->addNode(std::move(noise));
    vop->addNode(std::move(kf));
    vop->addNode(std::move(mul));
    vop->addNode(std::move(add));
    vop->addNode(std::move(geoOut));

    // Port 0 on geo_input is P (Vec3); port 0 on geo_output is also P.
    // geo_input.P → noise.input
    vop->createConnection(geoInUid, 0, noiseUid, 0);
    // noise.out → multiply.a
    vop->createConnection(noiseUid, 0, mulUid, 0);
    // constant_float.out → multiply.b
    vop->createConnection(kfUid, 0, mulUid, 1);
    // geo_input.P → add.a
    vop->createConnection(geoInUid, 0, addUid, 0);
    // multiply.out → add.b  (the perturbation along world axes — broadcasts to Vec3)
    vop->createConnection(mulUid, 0, addUid, 1);
    // add.out → geo_output.P (write back perturbed P)
    vop->createConnection(addUid, 0, geoOutUid, 0);

    // ── Cook the SOP graph ──
    sops::CookDiagnostic diag;
    auto emitted = sopGraph.cook(&diag);
    check(diag.ok, "sop graph cook ok");
    check(emitted.size() == 1, "exactly one EmittedActor");
    if (emitted.size() != 1)
    {
        std::printf("emitted size: %zu\n", emitted.size());
        return 1;
    }

    // The cube has 36 vertices / 12 triangles.
    check(emitted.front().geometry != nullptr, "emitted geometry shared_ptr populated");
    const auto &geo = *emitted.front().geometry;
    const auto &positions = geo.positions();
    check(positions.size() == 36, "emitted geometry has 36 positions");

    // Build a reference unaltered cube to compare against.
    SceneObject reference = SceneObject::createCube(2.0f);
    check(reference.vertexCount() == 36, "reference cube has 36 vertices");
    bool anyDelta = false;
    for (size_t i = 0; i < positions.size() && i < reference.positions().size(); ++i)
    {
        const auto &a = positions[i];
        const auto &b = reference.positions()[i];
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        if (dx * dx + dy * dy + dz * dz > 1e-8f) { anyDelta = true; break; }
    }
    check(anyDelta, "noise actually perturbed at least one position");

    // ── Round-trip the VopGraph through JSON ──
    const std::string json = vops::serializeVopGraph(*vop);
    auto parsed = vops::deserializeVopGraph(json);
    check(parsed != nullptr, "deserializeVopGraph returns a graph");
    if (parsed)
    {
        const std::string roundtrip = vops::serializeVopGraph(*parsed);
        check(roundtrip == json, "VopGraph JSON round-trips byte-stable");
    }

    // ── Round-trip the SOP graph (with embedded VopGraph) through JSON ──
    const std::string sopJson = sops::serializeSopGraph(sopGraph);
    auto parsedSop = sops::deserializeSopGraph(sopJson);
    check(parsedSop != nullptr, "deserializeSopGraph returns a graph");
    if (parsedSop)
    {
        const std::string sopRoundtrip = sops::serializeSopGraph(*parsedSop);
        check(sopRoundtrip == sopJson, "SOP graph (with VOP child) round-trips byte-stable");
    }

    if (failures == 0)
        std::printf("[attribute_vop_smoke] all checks passed\n");
    else
        std::printf("[attribute_vop_smoke] %d failure(s)\n", failures);
    return failures > 0 ? 1 : 0;
}
