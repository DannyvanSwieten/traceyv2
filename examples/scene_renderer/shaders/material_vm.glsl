// MaterialProgram VM (GPU side).
//
// Consumes four storage buffers declared at the pipeline layout level:
//   materialProgramCode.code[]            uvec4 array (one per Instruction, 16 bytes)
//   materialProgramConstants.constants[]  vec4 array (compile-time constants, immutable per program)
//   materialProgramHeaders.headers[]      uvec4 array, TWO entries per program:
//                                         headers[programIdx*2]   = (codeOffset, codeLength, constOffset, constLength)
//                                         headers[programIdx*2+1] = (paramOffset, paramCount, _pad, _pad)
//   materialParameters.parameters[]       vec4 array (animation-mutable parameter pool)
//
// Opcode values must stay in lockstep with src/shading/material_program/opcodes.hpp.
// If you add an opcode there, add it here too.

#ifndef MATERIAL_VM_GLSL
#define MATERIAL_VM_GLSL

// Opcodes ---------------------------------------------------------------------
#define MAT_OP_HALT                  0u
#define MAT_OP_LOAD_CONST            1u

#define MAT_OP_LOAD_POSITION         2u
#define MAT_OP_LOAD_NORMAL           3u
#define MAT_OP_LOAD_TANGENT          4u
#define MAT_OP_LOAD_VIEWDIR          5u
#define MAT_OP_LOAD_UV0              6u
#define MAT_OP_LOAD_UV1              7u

#define MAT_OP_LOAD_INPUT_ALBEDO     8u
#define MAT_OP_LOAD_INPUT_METALLIC   9u
#define MAT_OP_LOAD_INPUT_ROUGHNESS  10u
#define MAT_OP_LOAD_INPUT_EMISSION   11u
#define MAT_OP_LOAD_INPUT_NORMAL     12u

#define MAT_OP_ADD                   13u
#define MAT_OP_SUB                   14u
#define MAT_OP_MUL                   15u
#define MAT_OP_DIV                   16u
#define MAT_OP_NEG                   17u
#define MAT_OP_SATURATE              18u
#define MAT_OP_MIX                   19u
#define MAT_OP_CLAMP                 20u

#define MAT_OP_DOT3                  21u
#define MAT_OP_LENGTH3               22u
#define MAT_OP_CROSS                 23u
#define MAT_OP_NORMALIZE3            24u
#define MAT_OP_SPLAT                 25u

#define MAT_OP_WRITE_ALBEDO          26u
#define MAT_OP_WRITE_METALLIC        27u
#define MAT_OP_WRITE_ROUGHNESS       28u
#define MAT_OP_WRITE_EMISSION        29u
#define MAT_OP_WRITE_NORMAL          30u
#define MAT_OP_WRITE_ALPHA           31u
#define MAT_OP_WRITE_IOR             32u
#define MAT_OP_WRITE_TRANSMISSION    33u

#define MAT_OP_LOAD_PARAM            34u

#define MAT_VM_MAX_REGS              32

// Per-shading-point inputs the VM consumes.
struct MatInputs {
    vec3 albedo;
    float metallic;
    float roughness;
    vec3 emission;
    vec3 normal;        // tangent-space (or world-space, host's choice)
    vec3 viewDir;
    vec3 worldPosition;
    vec3 worldNormal;
    vec3 worldTangent;
    vec2 uv0;
    vec2 uv1;
};

// Result of evaluating a program. Mirrors MaterialEvalResult on the C++ side.
struct MatResult {
    vec3 albedo;
    float metallic;
    float roughness;
    vec3 emission;
    vec3 normal;
    float alpha;
    float ior;
    float transmission;
};

MatResult matResultDefault() {
    MatResult r;
    r.albedo = vec3(0.5);
    r.metallic = 0.0;
    r.roughness = 0.5;
    r.emission = vec3(0.0);
    r.normal = vec3(0.0, 0.0, 1.0);
    r.alpha = 1.0;
    r.ior = 1.5;
    r.transmission = 0.0;
    return r;
}

// Run the program at the given index against the supplied inputs.
MatResult runMaterialProgram(uint programIdx, MatInputs inp) {
    MatResult res = matResultDefault();

    // Two uvec4s per program: hdr0 carries code/const ranges, hdr1 carries
    // the parameter range (animation-mutable values).
    uvec4 hdr0 = materialProgramHeaders.headers[programIdx * 2u + 0u];
    uvec4 hdr1 = materialProgramHeaders.headers[programIdx * 2u + 1u];
    uint codeOffset  = hdr0.x;
    uint codeLength  = hdr0.y;
    uint constOffset = hdr0.z;
    // hdr0.w is constLength; not needed for execution but kept for symmetry.
    uint paramOffset = hdr1.x;
    // hdr1.y is paramCount; bounds-check is the compiler's responsibility.

    vec4 r[MAT_VM_MAX_REGS];
    for (uint i = 0u; i < uint(MAT_VM_MAX_REGS); ++i) {
        r[i] = vec4(0.0);
    }

    for (uint pc = 0u; pc < codeLength; ++pc) {
        uvec4 inst = materialProgramCode.code[codeOffset + pc];
        uint op   = inst.x & 0xFFFFu;
        uint dst  = (inst.x >> 16) & 0xFFFFu;
        uint sA   = inst.y & 0xFFFFu;
        uint sB   = (inst.y >> 16) & 0xFFFFu;
        uint sC   = inst.z & 0xFFFFu;
        uint imm  = (inst.z >> 16) & 0xFFFFu;

        if (op == MAT_OP_HALT) {
            break;
        } else if (op == MAT_OP_LOAD_CONST) {
            r[dst] = materialProgramConstants.constants[constOffset + imm];
        } else if (op == MAT_OP_LOAD_POSITION) {
            r[dst] = vec4(inp.worldPosition, 0.0);
        } else if (op == MAT_OP_LOAD_NORMAL) {
            r[dst] = vec4(inp.worldNormal, 0.0);
        } else if (op == MAT_OP_LOAD_TANGENT) {
            r[dst] = vec4(inp.worldTangent, 0.0);
        } else if (op == MAT_OP_LOAD_VIEWDIR) {
            r[dst] = vec4(inp.viewDir, 0.0);
        } else if (op == MAT_OP_LOAD_UV0) {
            r[dst] = vec4(inp.uv0, 0.0, 0.0);
        } else if (op == MAT_OP_LOAD_UV1) {
            r[dst] = vec4(inp.uv1, 0.0, 0.0);
        } else if (op == MAT_OP_LOAD_INPUT_ALBEDO) {
            r[dst] = vec4(inp.albedo, 0.0);
        } else if (op == MAT_OP_LOAD_INPUT_METALLIC) {
            r[dst] = vec4(inp.metallic);
        } else if (op == MAT_OP_LOAD_INPUT_ROUGHNESS) {
            r[dst] = vec4(inp.roughness);
        } else if (op == MAT_OP_LOAD_INPUT_EMISSION) {
            r[dst] = vec4(inp.emission, 0.0);
        } else if (op == MAT_OP_LOAD_INPUT_NORMAL) {
            r[dst] = vec4(inp.normal, 0.0);
        } else if (op == MAT_OP_ADD) {
            r[dst] = r[sA] + r[sB];
        } else if (op == MAT_OP_SUB) {
            r[dst] = r[sA] - r[sB];
        } else if (op == MAT_OP_MUL) {
            r[dst] = r[sA] * r[sB];
        } else if (op == MAT_OP_DIV) {
            r[dst] = r[sA] / r[sB];
        } else if (op == MAT_OP_NEG) {
            r[dst] = -r[sA];
        } else if (op == MAT_OP_SATURATE) {
            r[dst] = clamp(r[sA], vec4(0.0), vec4(1.0));
        } else if (op == MAT_OP_MIX) {
            r[dst] = mix(r[sA], r[sB], r[sC].x);
        } else if (op == MAT_OP_CLAMP) {
            r[dst] = clamp(r[sA], r[sB], r[sC]);
        } else if (op == MAT_OP_DOT3) {
            r[dst] = vec4(dot(r[sA].xyz, r[sB].xyz));
        } else if (op == MAT_OP_LENGTH3) {
            r[dst] = vec4(length(r[sA].xyz));
        } else if (op == MAT_OP_CROSS) {
            r[dst] = vec4(cross(r[sA].xyz, r[sB].xyz), 0.0);
        } else if (op == MAT_OP_NORMALIZE3) {
            r[dst] = vec4(normalize(r[sA].xyz), 0.0);
        } else if (op == MAT_OP_SPLAT) {
            r[dst] = vec4(r[sA].x);
        } else if (op == MAT_OP_WRITE_ALBEDO) {
            res.albedo = r[sA].xyz;
        } else if (op == MAT_OP_WRITE_METALLIC) {
            res.metallic = r[sA].x;
        } else if (op == MAT_OP_WRITE_ROUGHNESS) {
            res.roughness = r[sA].x;
        } else if (op == MAT_OP_WRITE_EMISSION) {
            res.emission = r[sA].xyz;
        } else if (op == MAT_OP_WRITE_NORMAL) {
            res.normal = r[sA].xyz;
        } else if (op == MAT_OP_WRITE_ALPHA) {
            res.alpha = r[sA].x;
        } else if (op == MAT_OP_WRITE_IOR) {
            res.ior = r[sA].x;
        } else if (op == MAT_OP_WRITE_TRANSMISSION) {
            res.transmission = r[sA].x;
        } else if (op == MAT_OP_LOAD_PARAM) {
            r[dst] = materialParameters.parameters[paramOffset + imm];
        }
    }

    return res;
}

#endif // MATERIAL_VM_GLSL
