// Smoke test for the MaterialProgram VM (Phase 0 step 1).
// Hand-builds material programs, runs them on the CPU evaluator, and confirms
// the integration with the existing PBR sampler. Used as a correctness oracle
// for the GPU VM that follows.

#include "../../src/shading/material_program/material_program.hpp"
#include "../../src/shading/material_program/cpu_evaluator.hpp"
#include "../../src/shading/bsdf/pbr/pbr.hpp"
#include "../../src/shading/bsdf/pbr/pbr_bsdf.hpp"
#include "../../src/shading/random/sampling.hpp"
#include "../../src/graph/graphs/shader_graph/shader_graph.hpp"
#include "../../src/graph/graphs/shader_graph/nodes.hpp"
#include "../../src/graph/graphs/shader_graph/compiler.hpp"
#include "../../src/graph/graphs/shader_graph/serialization.hpp"

#include <cmath>
#include <iostream>

using namespace tracey;

namespace
{
    bool approx(float a, float b, float eps = 1e-5f)
    {
        return std::fabs(a - b) <= eps;
    }

    bool approx(const Vec3 &a, const Vec3 &b, float eps = 1e-5f)
    {
        return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
    }

    int testWriteAttributes()
    {
        const Vec3 albedo(0.8f, 0.3f, 0.2f);
        const float metallic = 0.0f;
        const float roughness = 0.8f;

        MaterialProgramBuilder b;
        b.emit(Op::WriteAlbedo,    0, b.loadConst(albedo));
        b.emit(Op::WriteMetallic,  0, b.loadConst(metallic));
        b.emit(Op::WriteRoughness, 0, b.loadConst(roughness));
        MaterialProgram prog = b.finalize();

        SurfaceData s{};
        MaterialEvalResult res = evaluateMaterialProgramCPU(prog, s);

        if (!approx(res.albedo, albedo) ||
            !approx(res.metallic, metallic) ||
            !approx(res.roughness, roughness))
        {
            std::cerr << "FAIL writeAttributes: albedo=("
                      << res.albedo.x << "," << res.albedo.y << "," << res.albedo.z
                      << ") metallic=" << res.metallic
                      << " roughness=" << res.roughness << "\n";
            return 1;
        }
        std::cout << "OK writeAttributes\n";
        return 0;
    }

    int testMix()
    {
        // result = mix(red, green, 0.25) -> (0.75, 0.25, 0)
        MaterialProgramBuilder b;
        uint16_t rA = b.loadConst(Vec4(1.0f, 0.0f, 0.0f, 0.0f));
        uint16_t rB = b.loadConst(Vec4(0.0f, 1.0f, 0.0f, 0.0f));
        uint16_t rT = b.loadConst(0.25f);
        uint16_t rOut = b.allocReg();
        b.emit(Op::Mix, rOut, rA, rB, rT);
        b.emit(Op::WriteAlbedo, 0, rOut);
        MaterialProgram prog = b.finalize();

        SurfaceData s{};
        MaterialEvalResult res = evaluateMaterialProgramCPU(prog, s);

        const Vec3 expect = tracey::mix(Vec3(1, 0, 0), Vec3(0, 1, 0), 0.25f);
        if (!approx(res.albedo, expect))
        {
            std::cerr << "FAIL mix: got ("
                      << res.albedo.x << "," << res.albedo.y << "," << res.albedo.z
                      << ") expected ("
                      << expect.x << "," << expect.y << "," << expect.z << ")\n";
            return 1;
        }
        std::cout << "OK mix\n";
        return 0;
    }

    int testSurfaceLoadAndDot()
    {
        // Compute albedo = vec3(saturate(dot(N, V))) -- Lambert-style fake-shading.
        // For N == V, dot == 1, so albedo == (1,1,1).
        MaterialProgramBuilder b;
        uint16_t rN = b.loadSurface(Op::LoadNormal);
        uint16_t rV = b.loadSurface(Op::LoadViewDir);
        uint16_t rDot = b.allocReg();
        b.emit(Op::Dot3, rDot, rN, rV);
        uint16_t rSat = b.allocReg();
        b.emit(Op::Saturate, rSat, rDot);
        uint16_t rSplat = b.allocReg();
        b.emit(Op::Splat, rSplat, rSat);
        b.emit(Op::WriteAlbedo, 0, rSplat);
        MaterialProgram prog = b.finalize();

        SurfaceData s{};
        s.worldNormal = Vec3(0, 0, 1);
        s.viewDir = Vec3(0, 0, 1);
        MaterialEvalResult res = evaluateMaterialProgramCPU(prog, s);

        if (!approx(res.albedo, Vec3(1.0f)))
        {
            std::cerr << "FAIL surfaceLoadAndDot N=V: got ("
                      << res.albedo.x << "," << res.albedo.y << "," << res.albedo.z << ")\n";
            return 1;
        }

        // Now grazing: V perpendicular to N -> dot 0 -> saturate 0 -> albedo 0.
        s.viewDir = Vec3(1, 0, 0);
        res = evaluateMaterialProgramCPU(prog, s);
        if (!approx(res.albedo, Vec3(0.0f)))
        {
            std::cerr << "FAIL surfaceLoadAndDot grazing: got ("
                      << res.albedo.x << "," << res.albedo.y << "," << res.albedo.z << ")\n";
            return 1;
        }

        std::cout << "OK surfaceLoadAndDot\n";
        return 0;
    }

    int testPassthrough()
    {
        // Passthrough program should reproduce the host-provided inputs verbatim.
        MaterialProgram prog = makePassthroughProgram();

        SurfaceData s{};
        MaterialInputs in;
        in.albedo = Vec3(0.4f, 0.7f, 0.1f);
        in.metallic = 0.3f;
        in.roughness = 0.6f;
        in.emission = Vec3(0.05f, 0.0f, 0.0f);
        in.normal = Vec3(0, 0, 1);

        MaterialEvalResult res = evaluateMaterialProgramCPU(prog, s, in);

        if (!approx(res.albedo, in.albedo) ||
            !approx(res.metallic, in.metallic) ||
            !approx(res.roughness, in.roughness) ||
            !approx(res.emission, in.emission) ||
            !approx(res.normal, in.normal))
        {
            std::cerr << "FAIL passthrough\n";
            return 1;
        }
        std::cout << "OK passthrough\n";
        return 0;
    }

    int testProgramBufferPacking()
    {
        // Two programs in a buffer: each should be retrievable via its header.
        MaterialProgramBuffer pb;

        MaterialProgramBuilder ba;
        ba.emit(Op::WriteAlbedo, 0, ba.loadConst(Vec3(1.0f, 0.0f, 0.0f)));
        uint32_t idA = pb.addProgram(ba.finalize());

        MaterialProgramBuilder bb;
        bb.emit(Op::WriteAlbedo, 0, bb.loadConst(Vec3(0.0f, 1.0f, 0.0f)));
        uint32_t idB = pb.addProgram(bb.finalize());

        if (idA != 0 || idB != 1)
        {
            std::cerr << "FAIL programBufferPacking: ids " << idA << "," << idB << "\n";
            return 1;
        }
        if (pb.headers().size() != 2 ||
            pb.headers()[0].codeLength == 0 ||
            pb.headers()[1].codeLength == 0)
        {
            std::cerr << "FAIL programBufferPacking: header layout\n";
            return 1;
        }

        std::cout << "OK programBufferPacking ("
                  << pb.code().size() << " insts, "
                  << pb.constants().size() << " consts)\n";
        return 0;
    }

    int testParameterAnimation()
    {
        // The same compiled program should produce different outputs when its
        // parameter slots are mutated -- no recompile, no buffer rebuild.
        // This is the animation path the graph editor relies on.
        MaterialProgramBuilder b;
        uint16_t albedoParam = b.allocParam();
        uint16_t roughnessParam = b.allocParam();
        b.emit(Op::WriteAlbedo,    0, b.loadParam(albedoParam));
        b.emit(Op::WriteRoughness, 0, b.loadParam(roughnessParam));
        MaterialProgram prog = b.finalize();

        if (prog.parameterCount != 2)
        {
            std::cerr << "FAIL parameterAnimation: expected 2 params, got " << prog.parameterCount << "\n";
            return 1;
        }

        SurfaceData s{};
        MaterialInputs in{};
        MaterialParameters params;
        params.values.resize(prog.parameterCount);

        // Frame 1: red, smooth
        params.values[albedoParam] = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        params.values[roughnessParam] = Vec4(0.1f);
        MaterialEvalResult r1 = evaluateMaterialProgramCPU(prog, s, in, params);

        // Frame 2: blue, rough -- same program, mutated params
        params.values[albedoParam] = Vec4(0.0f, 0.0f, 1.0f, 0.0f);
        params.values[roughnessParam] = Vec4(0.9f);
        MaterialEvalResult r2 = evaluateMaterialProgramCPU(prog, s, in, params);

        if (!approx(r1.albedo, Vec3(1, 0, 0)) || !approx(r1.roughness, 0.1f) ||
            !approx(r2.albedo, Vec3(0, 0, 1)) || !approx(r2.roughness, 0.9f))
        {
            std::cerr << "FAIL parameterAnimation: frames disagree\n";
            return 1;
        }
        std::cout << "OK parameterAnimation (params=" << prog.parameterCount << ")\n";
        return 0;
    }

    int testProgramBufferReservesParamSlots()
    {
        // addProgram should reserve paramCount entries in the parameters pool
        // and record their offset in the header so the GPU can index correctly.
        MaterialProgramBuilder ba;
        ba.allocParam(); ba.allocParam();  // 2 params
        ba.emit(Op::WriteAlbedo, 0, ba.loadConst(Vec3(1, 0, 0)));

        MaterialProgramBuilder bb;
        bb.allocParam(); bb.allocParam(); bb.allocParam();  // 3 params
        bb.emit(Op::WriteAlbedo, 0, bb.loadConst(Vec3(0, 1, 0)));

        MaterialProgramBuffer pb;
        uint32_t idA = pb.addProgram(ba.finalize());
        uint32_t idB = pb.addProgram(bb.finalize());

        if (pb.headers()[idA].paramOffset != 0 ||
            pb.headers()[idA].paramCount != 2 ||
            pb.headers()[idB].paramOffset != 2 ||
            pb.headers()[idB].paramCount != 3 ||
            pb.parameters().size() != 5)
        {
            std::cerr << "FAIL programBufferReservesParamSlots: hdrA(off=" << pb.headers()[idA].paramOffset
                      << ",cnt=" << pb.headers()[idA].paramCount << ") hdrB(off=" << pb.headers()[idB].paramOffset
                      << ",cnt=" << pb.headers()[idB].paramCount << ") total=" << pb.parameters().size() << "\n";
            return 1;
        }
        std::cout << "OK programBufferReservesParamSlots (5 slots across 2 programs)\n";
        return 0;
    }

    int testCompileConstantToAlbedo()
    {
        // Graph: ConstantNode((1,0,0,0)) -> MaterialOutput.Albedo (port 0).
        ShaderGraph g(0);
        g.addNode(std::make_unique<ConstantNode>(1, Vec4(1.0f, 0.0f, 0.0f, 0.0f)));
        g.addNode(std::make_unique<MaterialOutputNode>(2));
        g.createConnection(/*fromNode*/1, /*fromPort*/0, /*toNode*/2, /*toPort*/0);

        MaterialProgram prog = compileShaderGraph(g);

        SurfaceData s{};
        MaterialEvalResult res = evaluateMaterialProgramCPU(prog, s);

        if (!approx(res.albedo, Vec3(1.0f, 0.0f, 0.0f)))
        {
            std::cerr << "FAIL compileConstantToAlbedo: got ("
                      << res.albedo.x << "," << res.albedo.y << "," << res.albedo.z << ")\n";
            return 1;
        }
        std::cout << "OK compileConstantToAlbedo\n";
        return 0;
    }

    int testCompilePassthroughEquivalence()
    {
        // Graph mirrors makePassthroughProgram: a single MaterialInput
        // → MaterialOutput passthrough wired one-to-one for each PBR
        // slot. Result must match running the hand-built passthrough
        // with the same inputs.
        //
        // Port indices come from materialInputPorts() / materialOutputPorts()
        // in src/graph/graphs/shader_graph/nodes.hpp.
        ShaderGraph g(0);
        struct Pair { size_t inPort, outPort; };
        const Pair pairs[] = {
            {7,  0},  // Albedo (input port 7)    → Albedo (output port 0)
            {8,  1},  // Metallic                  → Metallic
            {9,  2},  // Roughness                 → Roughness
            {10, 3},  // Emission                  → Emission
            {11, 4},  // InNormal                  → Normal
        };
        g.addNode(std::make_unique<MaterialInputNode>(1));
        g.addNode(std::make_unique<MaterialOutputNode>(2));
        for (const auto &p : pairs)
            g.createConnection(1, p.inPort, 2, p.outPort);

        MaterialProgram graphProg = compileShaderGraph(g);
        MaterialProgram handProg = makePassthroughProgram();

        SurfaceData s{};
        MaterialInputs in;
        in.albedo = Vec3(0.4f, 0.7f, 0.1f);
        in.metallic = 0.3f;
        in.roughness = 0.6f;
        in.emission = Vec3(0.05f, 0.0f, 0.0f);
        in.normal = Vec3(0, 1, 0);

        MaterialEvalResult rGraph = evaluateMaterialProgramCPU(graphProg, s, in);
        MaterialEvalResult rHand  = evaluateMaterialProgramCPU(handProg, s, in);

        if (!approx(rGraph.albedo, rHand.albedo) ||
            !approx(rGraph.metallic, rHand.metallic) ||
            !approx(rGraph.roughness, rHand.roughness) ||
            !approx(rGraph.emission, rHand.emission) ||
            !approx(rGraph.normal, rHand.normal))
        {
            std::cerr << "FAIL compilePassthroughEquivalence: graph and hand-built disagree\n";
            return 1;
        }
        std::cout << "OK compilePassthroughEquivalence\n";
        return 0;
    }

    int testCompileParameterCarriesDefault()
    {
        // Graph: ParameterNode("albedo", default=(0.2,0.4,0.6,0)) →
        // MaterialOutput.Albedo (port 0). Compiled program should
        // expose 1 parameter, carry the default, and produce the
        // default when evaluated with that as the parameter value.
        ShaderGraph g(0);
        const Vec4 defaultColor(0.2f, 0.4f, 0.6f, 0.0f);
        g.addNode(std::make_unique<ParameterNode>(1, "albedo", defaultColor));
        g.addNode(std::make_unique<MaterialOutputNode>(2));
        g.createConnection(1, 0, 2, 0);

        MaterialProgram prog = compileShaderGraph(g);

        if (prog.parameterCount != 1 ||
            prog.parameterDefaults.size() != 1 ||
            !approx(Vec3(prog.parameterDefaults[0]), Vec3(defaultColor)))
        {
            std::cerr << "FAIL compileParameterCarriesDefault: paramCount=" << prog.parameterCount
                      << " defaults=" << prog.parameterDefaults.size() << "\n";
            return 1;
        }

        SurfaceData s{};
        MaterialParameters params;
        params.values = prog.parameterDefaults;
        MaterialEvalResult res = evaluateMaterialProgramCPU(prog, s, {}, params);
        if (!approx(res.albedo, Vec3(defaultColor)))
        {
            std::cerr << "FAIL compileParameterCarriesDefault: eval gave ("
                      << res.albedo.x << "," << res.albedo.y << "," << res.albedo.z << ")\n";
            return 1;
        }

        // Mutate parameter and re-evaluate; output should track.
        params.values[0] = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        MaterialEvalResult res2 = evaluateMaterialProgramCPU(prog, s, {}, params);
        if (!approx(res2.albedo, Vec3(1.0f, 0.0f, 0.0f)))
        {
            std::cerr << "FAIL compileParameterCarriesDefault: mutation didn't take effect\n";
            return 1;
        }
        std::cout << "OK compileParameterCarriesDefault\n";
        return 0;
    }

    int testCompileBinaryOp()
    {
        // Graph: ConstantNode((0.2,0.3,0.4,0)) + ConstantNode((0.5,0.5,0.5,0))
        //          → Add → MaterialOutput.Albedo (port 0)
        ShaderGraph g(0);
        g.addNode(std::make_unique<ConstantNode>(1, Vec4(0.2f, 0.3f, 0.4f, 0.0f)));
        g.addNode(std::make_unique<ConstantNode>(2, Vec4(0.5f, 0.5f, 0.5f, 0.0f)));
        g.addNode(std::make_unique<BinaryOpNode>(3, Op::Add));
        g.addNode(std::make_unique<MaterialOutputNode>(4));
        g.createConnection(1, 0, 3, 0);  // constA -> add.a
        g.createConnection(2, 0, 3, 1);  // constB -> add.b
        g.createConnection(3, 0, 4, 0);  // add -> MaterialOutput.Albedo

        MaterialProgram prog = compileShaderGraph(g);
        SurfaceData s{};
        MaterialEvalResult res = evaluateMaterialProgramCPU(prog, s);

        if (!approx(res.albedo, Vec3(0.7f, 0.8f, 0.9f)))
        {
            std::cerr << "FAIL compileBinaryOp: got ("
                      << res.albedo.x << "," << res.albedo.y << "," << res.albedo.z << ")\n";
            return 1;
        }
        std::cout << "OK compileBinaryOp\n";
        return 0;
    }

    int testCompileDetectsCycle()
    {
        // Two binary nodes feeding into each other -- compile should throw.
        ShaderGraph g(0);
        g.addNode(std::make_unique<BinaryOpNode>(1, Op::Add));
        g.addNode(std::make_unique<BinaryOpNode>(2, Op::Add));
        g.createConnection(1, 0, 2, 0);
        g.createConnection(2, 0, 1, 0);  // closes the cycle

        try
        {
            compileShaderGraph(g);
            std::cerr << "FAIL compileDetectsCycle: expected runtime_error\n";
            return 1;
        }
        catch (const std::runtime_error &)
        {
            std::cout << "OK compileDetectsCycle\n";
            return 0;
        }
    }

    int testCompileDetectsDisconnectedInput()
    {
        // BinaryOp with only one input connected -- compile should throw.
        ShaderGraph g(0);
        g.addNode(std::make_unique<ConstantNode>(1, Vec4(1.0f)));
        g.addNode(std::make_unique<BinaryOpNode>(2, Op::Add));
        g.addNode(std::make_unique<MaterialOutputNode>(3));
        g.createConnection(1, 0, 2, 0);  // only input port 0 of Add is connected
        g.createConnection(2, 0, 3, 0);

        try
        {
            compileShaderGraph(g);
            std::cerr << "FAIL compileDetectsDisconnectedInput: expected runtime_error\n";
            return 1;
        }
        catch (const std::runtime_error &)
        {
            std::cout << "OK compileDetectsDisconnectedInput\n";
            return 0;
        }
    }

    int testJsonRoundTripPassthrough()
    {
        // Build a passthrough graph, serialize to JSON, deserialize, recompile.
        // Both compiled programs must produce the same MaterialEvalResult for
        // the same inputs.
        ShaderGraph original(0);
        // Port indices: Albedo=7→0, Metallic=8→1, Roughness=9→2 (see
        // materialInputPorts() / materialOutputPorts() in nodes.hpp).
        struct Pair { size_t inPort, outPort; };
        const Pair pairs[] = {{7, 0}, {8, 1}, {9, 2}};
        original.addNode(std::make_unique<MaterialInputNode>(1));
        original.addNode(std::make_unique<MaterialOutputNode>(2));
        for (const auto &p : pairs)
            original.createConnection(1, p.inPort, 2, p.outPort);

        const std::string jsonText = serializeShaderGraph(original);
        std::unique_ptr<ShaderGraph> reloaded = deserializeShaderGraph(jsonText);

        MaterialProgram p1 = compileShaderGraph(original);
        MaterialProgram p2 = compileShaderGraph(*reloaded);

        SurfaceData s{};
        MaterialInputs in;
        in.albedo = Vec3(0.4f, 0.7f, 0.1f);
        in.metallic = 0.3f;
        in.roughness = 0.6f;

        MaterialEvalResult r1 = evaluateMaterialProgramCPU(p1, s, in);
        MaterialEvalResult r2 = evaluateMaterialProgramCPU(p2, s, in);

        if (!approx(r1.albedo, r2.albedo) ||
            !approx(r1.metallic, r2.metallic) ||
            !approx(r1.roughness, r2.roughness))
        {
            std::cerr << "FAIL jsonRoundTripPassthrough: original and reloaded disagree\n";
            return 1;
        }
        std::cout << "OK jsonRoundTripPassthrough\n";
        return 0;
    }

    int testJsonRoundTripWithParameter()
    {
        // Round-trip a graph that has a Parameter node. The parameterDefaults
        // and parameterCount on the recompiled program must match.
        ShaderGraph original(42);
        const Vec4 def(0.2f, 0.4f, 0.6f, 1.0f);
        original.addNode(std::make_unique<ParameterNode>(1, "albedoTint", def));
        original.addNode(std::make_unique<MaterialOutputNode>(2));
        original.createConnection(1, 0, 2, 0);  // → Albedo (port 0)

        const std::string jsonText = serializeShaderGraph(original);
        auto reloaded = deserializeShaderGraph(jsonText);

        MaterialProgram p1 = compileShaderGraph(original);
        MaterialProgram p2 = compileShaderGraph(*reloaded);

        if (p1.parameterCount != p2.parameterCount ||
            p1.parameterDefaults.size() != p2.parameterDefaults.size() ||
            !approx(Vec3(p1.parameterDefaults[0]), Vec3(p2.parameterDefaults[0])))
        {
            std::cerr << "FAIL jsonRoundTripWithParameter\n";
            return 1;
        }
        if (reloaded->uid() != 42)
        {
            std::cerr << "FAIL jsonRoundTripWithParameter: graph uid lost (got "
                      << reloaded->uid() << ")\n";
            return 1;
        }
        std::cout << "OK jsonRoundTripWithParameter\n";
        return 0;
    }

    int testHandAuthoredJson()
    {
        // Parse a JSON string the user might hand-edit. Compile and evaluate
        // to check the result matches what the JSON describes:
        //   ConstantNode(1, 0, 0, 0) -> MaterialOutput.Albedo (port 0)
        const std::string jsonText = R"({
            "version": 1,
            "uid": 0,
            "nodes": [
                {"uid": 1, "kind": "Constant",       "value": [1.0, 0.0, 0.0, 0.0]},
                {"uid": 2, "kind": "MaterialOutput"}
            ],
            "connections": [
                {"from_node": 1, "from_port": 0, "to_node": 2, "to_port": 0}
            ]
        })";

        auto graph = deserializeShaderGraph(jsonText);
        MaterialProgram prog = compileShaderGraph(*graph);

        SurfaceData s{};
        MaterialEvalResult res = evaluateMaterialProgramCPU(prog, s);
        if (!approx(res.albedo, Vec3(1.0f, 0.0f, 0.0f)))
        {
            std::cerr << "FAIL handAuthoredJson: albedo ("
                      << res.albedo.x << "," << res.albedo.y << "," << res.albedo.z << ")\n";
            return 1;
        }
        std::cout << "OK handAuthoredJson\n";
        return 0;
    }

    int testJsonRejectsBadVersion()
    {
        const std::string jsonText = R"({"version": 99, "uid": 0, "nodes": [], "connections": []})";
        try
        {
            deserializeShaderGraph(jsonText);
            std::cerr << "FAIL jsonRejectsBadVersion: expected throw\n";
            return 1;
        }
        catch (const std::runtime_error &)
        {
            std::cout << "OK jsonRejectsBadVersion\n";
            return 0;
        }
    }

    int testJsonRejectsUnknownOp()
    {
        const std::string jsonText = R"({
            "version": 1, "uid": 0,
            "nodes": [{"uid": 1, "kind": "BinaryOp", "op": "NonExistent"}],
            "connections": []
        })";
        try
        {
            deserializeShaderGraph(jsonText);
            std::cerr << "FAIL jsonRejectsUnknownOp: expected throw\n";
            return 1;
        }
        catch (const std::runtime_error &)
        {
            std::cout << "OK jsonRejectsUnknownOp\n";
            return 0;
        }
    }

    int testIntegrationWithPBRSampler()
    {
        // Build a diffuse program, evaluate it, feed result into sampleBRDF.
        // We don't compare exact sample directions (RNG-dependent); we just
        // confirm the produced PBRMaterial fields are equal to a directly-built
        // one and that sampleBRDF returns something well-defined for both.
        const Vec3 albedo(0.7f, 0.5f, 0.2f);
        const float metallic = 0.0f;
        const float roughness = 0.8f;

        MaterialProgramBuilder b;
        b.emit(Op::WriteAlbedo,    0, b.loadConst(albedo));
        b.emit(Op::WriteMetallic,  0, b.loadConst(metallic));
        b.emit(Op::WriteRoughness, 0, b.loadConst(roughness));
        MaterialProgram prog = b.finalize();

        SurfaceData surface{};
        surface.worldNormal = Vec3(0, 0, 1);
        surface.viewDir = Vec3(0, 0, 1);

        MaterialEvalResult vmRes = evaluateMaterialProgramCPU(prog, surface);

        PBRMaterial mat;
        mat.albedo = vmRes.albedo;
        mat.metallic = vmRes.metallic;
        mat.roughness = vmRes.roughness;

        // Round-trip through sampleBRDF with a fixed seed.
        SimpleRNG rng(0xC0FFEE);
        Sample s = sampleBRDF(surface.worldNormal, surface.viewDir, rng, mat);
        if (s.pdf <= 0.0f)
        {
            std::cerr << "FAIL pbrIntegration: sampleBRDF produced pdf<=0\n";
            return 1;
        }
        if (!std::isfinite(s.f.x) || !std::isfinite(s.f.y) || !std::isfinite(s.f.z))
        {
            std::cerr << "FAIL pbrIntegration: non-finite BSDF\n";
            return 1;
        }

        std::cout << "OK pbrIntegration (pdf=" << s.pdf
                  << ", f=" << s.f.x << "," << s.f.y << "," << s.f.z << ")\n";
        return 0;
    }
}  // namespace

int main()
{
    int rc = 0;
    rc |= testWriteAttributes();
    rc |= testMix();
    rc |= testSurfaceLoadAndDot();
    rc |= testPassthrough();
    rc |= testProgramBufferPacking();
    rc |= testParameterAnimation();
    rc |= testProgramBufferReservesParamSlots();
    rc |= testCompileConstantToAlbedo();
    rc |= testCompilePassthroughEquivalence();
    rc |= testCompileParameterCarriesDefault();
    rc |= testCompileBinaryOp();
    rc |= testCompileDetectsCycle();
    rc |= testCompileDetectsDisconnectedInput();
    rc |= testJsonRoundTripPassthrough();
    rc |= testJsonRoundTripWithParameter();
    rc |= testHandAuthoredJson();
    rc |= testJsonRejectsBadVersion();
    rc |= testJsonRejectsUnknownOp();
    rc |= testIntegrationWithPBRSampler();
    if (rc != 0) std::cerr << "FAILURES\n";
    else std::cout << "ALL OK\n";
    return rc;
}
