#pragma once
#include "opcodes.hpp"
#include "../../core/types.hpp"
#include <vector>

namespace tracey
{
    // Per-shading-point inputs to the VM.
    struct SurfaceData
    {
        Vec3 worldPosition{0.0f};
        Vec3 worldNormal{0.0f, 0.0f, 1.0f};
        Vec3 worldTangent{1.0f, 0.0f, 0.0f};
        Vec3 viewDir{0.0f, 0.0f, 1.0f};
        Vec2 uv0{0.0f};
        Vec2 uv1{0.0f};
        uint32_t primitiveIndex = 0;
        uint32_t instanceIndex = 0;
        uint32_t materialIndex = 0;
    };

    // Pre-fetched material attributes the host pipeline feeds into the VM.
    // The graph can either pass these through (default behaviour) or override them.
    // Texture sampling lives in the host shader for now; a future SampleTexture2D
    // opcode will let graphs sample textures themselves.
    struct MaterialInputs
    {
        Vec3 albedo{0.5f};
        float metallic = 0.0f;
        float roughness = 0.5f;
        Vec3 emission{0.0f};
        Vec3 normal{0.0f, 0.0f, 1.0f};
    };

    // Mutable parameter slice for a single material instance. Read by Op::LoadParam.
    // The graph compiler decides how many slots a program exposes; the host
    // updates these values per frame for animation without recompiling.
    struct MaterialParameters
    {
        std::vector<Vec4> values;  // one slot per exposed parameter
    };

    // PBR attribute set produced by a MaterialProgram. Mirrors PBRMaterial so
    // the existing sampleBRDF/evalBRDF code can consume it directly.
    struct MaterialEvalResult
    {
        Vec3 albedo{0.5f};
        float metallic = 0.0f;
        float roughness = 0.5f;
        Vec3 emission{0.0f};
        Vec3 normal{0.0f, 0.0f, 1.0f};
        float alpha = 1.0f;
        float ior = 1.5f;
        float transmission = 0.0f;
    };

    // A compiled material: flat bytecode + a vec4 constant pool + a count
    // of mutable parameter slots (the graph's animation surface) along with
    // their default values.
    //
    // parameterDefaults: optional. If non-empty, must be exactly parameterCount
    // entries. MaterialProgramBuffer::addProgram uses these to seed the
    // parameters pool; otherwise slots are zero-initialised and the host is
    // expected to populate them.
    struct MaterialProgram
    {
        std::vector<Instruction> code;
        std::vector<Vec4> constants;
        std::vector<Vec4> parameterDefaults;
        uint16_t parameterCount = 0;
        uint8_t registerCount = 0;
    };

    // Convenience emitter for hand-built and graph-compiled programs.
    // Tracks register and constant allocation; not the optimizing compiler.
    class MaterialProgramBuilder
    {
    public:
        // Allocate a new register slot. Throws if the register file is exhausted.
        uint16_t allocReg();

        // Append a vec4 constant; returns its slot index for use with LoadConst.
        uint16_t addConstant(const Vec4 &c);
        uint16_t addConstant(const Vec3 &c, float w = 0.0f) { return addConstant(Vec4(c, w)); }
        uint16_t addConstant(float c) { return addConstant(Vec4(c)); }

        // Emit a raw instruction. Operand meanings are op-dependent (see Op).
        void emit(Op op,
                  uint16_t dst = 0,
                  uint16_t srcA = 0,
                  uint16_t srcB = 0,
                  uint16_t srcC = 0,
                  uint16_t imm = 0);

        // Common patterns ------------------------------------------------------

        uint16_t loadConst(uint16_t constIdx)
        {
            uint16_t r = allocReg();
            emit(Op::LoadConst, r, 0, 0, 0, constIdx);
            return r;
        }
        uint16_t loadConst(const Vec4 &v) { return loadConst(addConstant(v)); }
        uint16_t loadConst(const Vec3 &v, float w = 0.0f) { return loadConst(addConstant(v, w)); }
        uint16_t loadConst(float v) { return loadConst(addConstant(v)); }

        // Emit a surface-attribute load (LoadPosition, LoadNormal, ...).
        uint16_t loadSurface(Op surfaceOp);

        // Allocate a fresh parameter slot. Returns its index (0..parameterCount-1)
        // for use with loadParam / Op::LoadParam.
        uint16_t allocParam();

        // Emit Op::LoadParam reading parameter slot paramIdx into a fresh register.
        uint16_t loadParam(uint16_t paramIdx);

        // Append a Halt (if not already present) and return the built program.
        MaterialProgram finalize();

    private:
        std::vector<Instruction> m_code;
        std::vector<Vec4> m_constants;
        uint16_t m_nextReg = 0;
        uint16_t m_nextParam = 0;
    };

    // Default program: copies host-provided MaterialInputs into the result verbatim.
    // Used for materials that don't yet have a graph attached.
    MaterialProgram makePassthroughProgram();

    // Packs N programs into flat arrays ready to be uploaded as SSBOs.
    // Layout is shared between CPU and GPU.
    //
    // Header is 32 bytes (2 uvec4s on the GPU). The first uvec4 carries the
    // immutable code/constants ranges; the second carries the mutable parameter
    // range. The host updates `parameters[paramOffset..paramOffset+paramCount]`
    // for animation; the program code itself never changes for parameter edits.
    class MaterialProgramBuffer
    {
    public:
        struct Header
        {
            uint32_t codeOffset;
            uint32_t codeLength;
            uint32_t constOffset;
            uint32_t constLength;
            uint32_t paramOffset;
            uint32_t paramCount;
            uint32_t _pad0;
            uint32_t _pad1;
        };
        static_assert(sizeof(Header) == 32, "Header must be 2 uvec4s");

        // Append a program. Reserves parameterCount slots in the parameters
        // pool, initialised to zero (host can overwrite via parameters()).
        // Returns the new program's ID (0-indexed).
        uint32_t addProgram(const MaterialProgram &p);

        void clear();

        // Byte sizes for sizing the GPU buffers. May be zero if no programs added.
        size_t codeBytes() const { return m_code.size() * sizeof(Instruction); }
        size_t constantsBytes() const { return m_constants.size() * sizeof(Vec4); }
        size_t headersBytes() const { return m_headers.size() * sizeof(Header); }
        size_t parametersBytes() const { return m_parameters.size() * sizeof(Vec4); }

        const std::vector<Instruction> &code() const { return m_code; }
        const std::vector<Vec4> &constants() const { return m_constants; }
        const std::vector<Header> &headers() const { return m_headers; }
        const std::vector<Vec4> &parameters() const { return m_parameters; }

        // Mutable access to parameters for animation. Index by absolute slot
        // (header.paramOffset + paramIdx within program).
        std::vector<Vec4> &parameters() { return m_parameters; }

    private:
        std::vector<Instruction> m_code;
        std::vector<Vec4> m_constants;
        std::vector<Header> m_headers;
        std::vector<Vec4> m_parameters;
    };
}
