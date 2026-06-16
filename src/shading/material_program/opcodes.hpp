#pragma once
#include <cstdint>

namespace tracey
{
    // MaterialProgram VM opcodes. The VM evaluates a program against per-shading-point
    // SurfaceData and produces a MaterialEvalResult (a PBR attribute set). BRDF
    // sampling/eval then runs as a separate step using the existing PBR code.
    //
    // All registers are vec4. Ops that operate on narrower types (float, vec2, vec3)
    // read/write only the relevant lanes; remaining lanes are unspecified.
    //
    // Operand semantics in Instruction (see below) are op-dependent. Documented per op.
    enum class Op : uint16_t
    {
        // End of program. Required terminator.
        Halt = 0,

        // dst = constants[imm]. srcA/srcB/srcC unused.
        LoadConst,

        // dst <- surface attribute (xyz or xy as appropriate). No operands.
        LoadPosition,
        LoadNormal,
        LoadTangent,
        LoadViewDir,
        LoadUV0,
        LoadUV1,

        // dst <- pre-fetched material attribute (the host shader fetches textures/factors
        // and feeds them in as MaterialInputs; the graph can pass them through or override).
        LoadInputAlbedo,
        LoadInputMetallic,
        LoadInputRoughness,
        LoadInputEmission,
        LoadInputNormal,

        // dst = op(srcA, srcB), per-component vec4.
        Add,
        Sub,
        Mul,
        Div,

        // dst = -srcA.
        Neg,

        // dst = saturate(srcA).
        Saturate,

        // dst = mix(srcA, srcB, srcC.x).
        Mix,

        // dst = clamp(srcA, srcB, srcC), per-component.
        Clamp,

        // dst.x = dot(srcA.xyz, srcB.xyz). Other lanes unspecified.
        Dot3,

        // dst.x = length(srcA.xyz). Other lanes unspecified.
        Length3,

        // dst.xyz = cross(srcA.xyz, srcB.xyz).
        Cross,

        // dst.xyz = normalize(srcA.xyz).
        Normalize3,

        // dst = vec4(srcA.x).
        Splat,

        // Outputs into MaterialEvalResult. srcA carries the value; dst unused.
        WriteAlbedo,
        WriteMetallic,
        WriteRoughness,
        WriteEmission,
        WriteNormal,
        WriteAlpha,
        WriteIor,
        WriteTransmission,

        // dst = parameters[paramBase + imm]. The host fills the parameters
        // pool from the graph's exposed parameter surface; values change at
        // animation rate without recompiling the program.
        LoadParam,

        // dst = vec4(float(surface.instanceIndex)). Splatted across all
        // four lanes so it composes with arithmetic / Mix / Splat ops the
        // same way any other surface load does. The hit shader stamps the
        // per-instance TLAS index into SurfaceData::instanceIndex.
        //
        // Appended at the end (not next to the other surface loads) so the
        // numeric Op values that already exist on disk and in the GPU
        // material VM stay stable.
        LoadInstanceIndex,

        // dst <- pre-fetched transparency/IOR/opacity inputs (the host fetches
        // GPUMaterial.transmissionFactor / iorFactor / baseColorA and feeds
        // them in; passthrough copies them to WriteTransmission/WriteIor/
        // WriteAlpha). Appended at the end so existing numeric Op values
        // (which the GPU/CPU VMs hardcode) stay stable.
        LoadInputTransmission,
        LoadInputIor,
        LoadInputOpacity,

        Count_
    };

    // Fixed 16-byte instruction. Maps to a uvec4 on the GPU (std430-friendly).
    //
    // Field meanings depend on op:
    //   - LoadConst:       imm = constant index; srcA/srcB/srcC unused.
    //   - LoadX (surface): no operands.
    //   - Binary math:     dst = op(srcA, srcB).
    //   - Mix/Clamp:       dst = op(srcA, srcB, srcC).
    //   - WriteX:          srcA holds the value; dst unused.
    struct Instruction
    {
        uint16_t op;
        uint16_t dst;
        uint16_t srcA;
        uint16_t srcB;
        uint16_t srcC;
        uint16_t imm;
        uint32_t aux;
    };
    static_assert(sizeof(Instruction) == 16, "Instruction must be 16 bytes (uvec4-equivalent)");

    // Hard caps. Mirror these in the GPU-side VM.
    constexpr uint32_t kMaxRegisters = 32;
    constexpr uint32_t kMaxInstructions = 256;
}
