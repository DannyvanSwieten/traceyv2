// MSL source for the Metal RT path tracer megakernel.
//
// Line-by-line port of the canonical GLSL shader set in
// examples/scene_renderer/shaders/ — the same set the editor always runs:
//   ray_gen.glsl     → cameraRay() + seed/jitter logic
//   uber_hit.glsl    → shadeHit() (material VM, NEE, glass/metal/diffuse)
//   sky_miss.glsl    → skyRadiance()
//   resolve.glsl     → Welford mean + Reinhard + gamma at the kernel tail
//   pbr_lib.glsl     → RNG / sampling / fresnel / GGX / material fetch /
//                      UV + normal interpolation
//   material_vm.glsl → runMaterialProgram() (+ op 35 LoadInstanceIndex,
//                      which exists in opcodes.hpp but was missing from
//                      the GLSL VM)
//
// The RNG (hash / nextRandom) and the per-(pixel, sample) seed formula are
// bit-exact ports so the Metal and wavefront renderers walk identical
// sample sequences — that's what makes the parity harness meaningful.
//
// Buffer index map (must match MetalPathTracerBackend::dispatch):
//   0  Uniforms (setBytes)
//   1  lights            float4[]  (6 per light — GPULight)
//   2  materials         int[]     (20 per instance — GPUMaterial, aliased)
//   3  instanceData      uint2[]   (.x programId, .y uv/vertex base offset)
//   4  uvs               float2[]  (global per-vertex, BLAS-concatenated)
//   5  normals           float4[]  (global per-vertex, BLAS-concatenated)
//   6  positions         packed_float3[] (global per-vertex, BLAS-concatenated)
//   7  normalMats        float4[]  (3 rows per instance: inverse-transpose 3x3)
//   8  progCode          uint4[]
//   9  progConst         float4[]
//   10 progHeaders       uint4[]   (2 per program)
//   11 progParams        float4[]
//   12 accumBuffer       float4[]  (width*height running mean, read-write)
//   13 accel             instance_acceleration_structure
//   14 emitters          float4[]  (4 per emissive tri — NEE)
//   15..20 AOV layers    float4[]  (albedo/normal/depth/position/emission/id;
//                                   written only when Uniforms.enableAovs)
// Texture index map:
//   0      outImage (write)
//   1..N   scene textures (array<texture2d<float>, kMaxTextures>)

#pragma once

namespace tracey
{
    inline constexpr unsigned kMetalRTMaxTextures = 64;

    inline const char *kPathtraceMSL = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace metal::raytracing;

constant uint kMaxTextures = 64;

struct Uniforms {
    float3 camPos;
    float3 camFwd;
    float3 camRight;
    float3 camUp;
    float  fovDegrees;
    uint   maxDepth;
    int    currentSample;   // 1-based count of render() calls since clear
    uint   lightCount;
    uint   samplesPerFrame;
    uint   width;
    uint   height;
    uint   emitterCount;  // reuses the host struct's former trailing pad (offset 92)
    uint   enableAovs;    // 0/1 — gates the AOV-layer writes (offset 96)
    uint   linearOutput;  // 0/1 — skip tonemap+gamma, emit linear (offset 100)
    float  aperture;      // thin-lens radius (offset 104; 0 = pinhole)
    float  focalDistance; // in-focus distance along the view dir (offset 108)
};

// ── RNG (bit-exact ports of ray_gen.glsl hash / pbr_lib.glsl nextRandom) ──

float hashSeed(uint seed) {
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / 4294967296.0;
}

float nextRandom(thread uint &seed) {
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / 4294967296.0;
}

// ── Sampling / tangent frame / fresnel / GGX (pbr_lib.glsl) ──────────────

constant float PI = 3.14159265359;

float3 sampleCosineHemisphere(float r1, float r2) {
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt(r2);
    float sinTheta = sqrt(1.0 - r2);
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 sampleGGX(float r1, float r2, float roughness) {
    float a = roughness * roughness;
    float a2 = max(a * a, 0.0001);
    float phi = 2.0 * PI * r1;
    float denom = max(1.0 + (a2 - 1.0) * r2, 0.0001);
    float cosTheta = sqrt((1.0 - r2) / denom);
    cosTheta = clamp(cosTheta, 0.0, 1.0);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Anisotropic GGX half-vector sample in tangent space (x=T, y=B, z=N). At
// aT==aB it reduces exactly to sampleGGX (azimuth → 2π r1, cosθ identical).
float3 sampleGGXAniso(float r1, float r2, float aT, float aB) {
    float phi = atan2(aB * sin(2.0 * PI * r1), aT * cos(2.0 * PI * r1));
    float cosPhi = cos(phi), sinPhi = sin(phi);
    float A = (cosPhi * cosPhi) / max(aT * aT, 1e-8) +
              (sinPhi * sinPhi) / max(aB * aB, 1e-8);
    float tanTheta2 = r2 / max((1.0 - r2) * A, 1e-8);
    float cosTheta = 1.0 / sqrt(1.0 + tanTheta2);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return float3(cosPhi * sinTheta, sinPhi * sinTheta, cosTheta);
}

void buildTangentFrame(float3 N, thread float3 &T, thread float3 &B) {
    float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// UV-aligned tangent (Lengyel), Gram-Schmidt orthonormalized against N. Falls
// back to `fallbackT` for degenerate UVs. Sign of dPdu is irrelevant — the
// anisotropic lobe is symmetric under T → -T.
float3 computeUVTangent(float3 p0, float3 p1, float3 p2,
                        float2 uv0, float2 uv1, float2 uv2,
                        float3 N, float3 fallbackT) {
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float2 d1 = uv1 - uv0, d2 = uv2 - uv0;
    float det = d1.x * d2.y - d2.x * d1.y;
    if (abs(det) < 1e-12) return fallbackT;
    float3 Traw = e1 * d2.y - e2 * d1.y;
    Traw = Traw - N * dot(N, Traw);
    float l = length(Traw);
    return l > 1e-8 ? Traw / l : fallbackT;
}

float3 tangentToWorld(float3 v, float3 N, float3 T, float3 B) {
    return v.x * T + v.y * B + v.z * N;
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

float fresnelDielectric(float cosTheta, float etaI, float etaT) {
    float r0 = (etaI - etaT) / (etaI + etaT);
    r0 = r0 * r0;
    float c = 1.0 - cosTheta;
    float c2 = c * c;
    float c5 = c2 * c2 * c;
    return r0 + (1.0 - r0) * c5;
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float denom = NdotV * (1.0 - k) + k;
    return NdotV / max(denom, 0.0001);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotL, roughness) * geometrySchlickGGX(NdotV, roughness);
}

// ── Material fetch (pbr_lib.glsl, GPUMaterial as int[28]) ────────────────

constant uint MATERIAL_STRIDE = 28;

// GLSL dispatches over 4 bound sampler objects; MSL constexpr samplers give
// us the same four combinations without binding anything.
float4 sampleMatTexture(array<texture2d<float>, kMaxTextures> textures,
                        uint texIdx, uint kind, float2 uv) {
    constexpr sampler linearRepeat(filter::linear, address::repeat);
    constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);
    constexpr sampler nearestRepeat(filter::nearest, address::repeat);
    constexpr sampler nearestClamp(filter::nearest, address::clamp_to_edge);
    if (texIdx >= kMaxTextures) return float4(1.0);
    if (kind == 1u) return textures[texIdx].sample(linearClamp,   uv, level(0.0));
    if (kind == 2u) return textures[texIdx].sample(nearestRepeat, uv, level(0.0));
    if (kind == 3u) return textures[texIdx].sample(nearestClamp,  uv, level(0.0));
    return textures[texIdx].sample(linearRepeat, uv, level(0.0));
}

uint samplerKindForSlot(device const int *materials, uint instanceIndex, uint slot) {
    uint baseOffset = instanceIndex * MATERIAL_STRIDE;
    uint bits = uint(materials[baseOffset + 5u]);
    return (bits >> (slot * 2u)) & 0x3u;
}

float3 getMaterialAlbedo(device const int *materials,
                         array<texture2d<float>, kMaxTextures> textures,
                         uint instanceIndex, float2 uv) {
    uint baseOffset = instanceIndex * MATERIAL_STRIDE;
    int albedoTexIdx = materials[baseOffset + 0u];
    float3 baseColor = float3(as_type<float>(materials[baseOffset + 8u]),
                              as_type<float>(materials[baseOffset + 9u]),
                              as_type<float>(materials[baseOffset + 10u]));
    if (albedoTexIdx >= 0) {
        float4 texColor = sampleMatTexture(textures, uint(albedoTexIdx),
                                           samplerKindForSlot(materials, instanceIndex, 0u), uv);
        return texColor.rgb * baseColor;
    }
    return baseColor;
}

float2 getMaterialMetallicRoughness(device const int *materials,
                                    array<texture2d<float>, kMaxTextures> textures,
                                    uint instanceIndex, float2 uv) {
    uint baseOffset = instanceIndex * MATERIAL_STRIDE;
    int mrTexIdx = materials[baseOffset + 2u];
    float metallicFactor  = as_type<float>(materials[baseOffset + 12u]);
    float roughnessFactor = as_type<float>(materials[baseOffset + 13u]);
    if (mrTexIdx >= 0) {
        float4 mrTex = sampleMatTexture(textures, uint(mrTexIdx),
                                        samplerKindForSlot(materials, instanceIndex, 2u), uv);
        // glTF spec: G=roughness, B=metallic
        return float2(mrTex.b * metallicFactor, mrTex.g * roughnessFactor);
    }
    return float2(metallicFactor, roughnessFactor);
}

float3 getMaterialEmissive(device const int *materials,
                           array<texture2d<float>, kMaxTextures> textures,
                           uint instanceIndex, float2 uv) {
    uint baseOffset = instanceIndex * MATERIAL_STRIDE;
    int emissiveTexIdx = materials[baseOffset + 3u];
    float3 emissiveFactor = float3(as_type<float>(materials[baseOffset + 14u]),
                                   as_type<float>(materials[baseOffset + 15u]),
                                   as_type<float>(materials[baseOffset + 16u]));
    if (emissiveTexIdx >= 0) {
        float4 texColor = sampleMatTexture(textures, uint(emissiveTexIdx),
                                           samplerKindForSlot(materials, instanceIndex, 3u), uv);
        return texColor.rgb * emissiveFactor;
    }
    return emissiveFactor;
}

// Transparency / emission scalars packed at fixed offsets in GPUMaterial.
// (11=opacity/baseColorA, 17=transmission, 18=ior, 19=emissiveStrength.)
float getMaterialOpacity(device const int *materials, uint instanceIndex) {
    return as_type<float>(materials[instanceIndex * MATERIAL_STRIDE + 11u]);
}
float getMaterialTransmission(device const int *materials, uint instanceIndex) {
    return as_type<float>(materials[instanceIndex * MATERIAL_STRIDE + 17u]);
}
float getMaterialIor(device const int *materials, uint instanceIndex) {
    return as_type<float>(materials[instanceIndex * MATERIAL_STRIDE + 18u]);
}
float getMaterialEmissiveStrength(device const int *materials, uint instanceIndex) {
    return as_type<float>(materials[instanceIndex * MATERIAL_STRIDE + 19u]);
}
// R3 advanced-BSDF lobe factors (offsets 20-23; see GPUMaterial).
float getMaterialClearcoat(device const int *materials, uint i) {
    return as_type<float>(materials[i * MATERIAL_STRIDE + 20u]);
}
float getMaterialClearcoatRoughness(device const int *materials, uint i) {
    return as_type<float>(materials[i * MATERIAL_STRIDE + 21u]);
}
float getMaterialSheen(device const int *materials, uint i) {
    return as_type<float>(materials[i * MATERIAL_STRIDE + 22u]);
}
float getMaterialAnisotropy(device const int *materials, uint i) {
    return as_type<float>(materials[i * MATERIAL_STRIDE + 23u]);
}
// R3d subsurface: weight (24) + scatter-tint RGB (25-27).
float getMaterialSubsurface(device const int *materials, uint i) {
    return as_type<float>(materials[i * MATERIAL_STRIDE + 24u]);
}
float3 getMaterialSubsurfaceColor(device const int *materials, uint i) {
    uint b = i * MATERIAL_STRIDE;
    return float3(as_type<float>(materials[b + 25u]),
                  as_type<float>(materials[b + 26u]),
                  as_type<float>(materials[b + 27u]));
}

// ── Material VM (material_vm.glsl + op 35 from opcodes.hpp) ──────────────

struct MatInputs {
    float3 albedo;
    float  metallic;
    float  roughness;
    float3 emission;
    float3 normal;
    float3 viewDir;
    float3 worldPosition;
    float3 worldNormal;
    float3 worldTangent;
    float2 uv0;
    float2 uv1;
    uint   instanceIndex;
    float  transmission;
    float  ior;
    float  opacity;
};

struct MatResult {
    float3 albedo;
    float  metallic;
    float  roughness;
    float3 emission;
    float3 normal;
    float  alpha;
    float  ior;
    float  transmission;
};

#define MAT_VM_MAX_REGS 32

MatResult runMaterialProgram(uint programIdx, MatInputs inp,
                             device const uint4 *code,
                             device const float4 *constantsBuf,
                             device const uint4 *headers,
                             device const float4 *params) {
    MatResult res;
    res.albedo = float3(0.5);
    res.metallic = 0.0;
    res.roughness = 0.5;
    res.emission = float3(0.0);
    res.normal = float3(0.0, 0.0, 1.0);
    res.alpha = 1.0;
    res.ior = 1.5;
    res.transmission = 0.0;

    uint4 hdr0 = headers[programIdx * 2u + 0u];
    uint4 hdr1 = headers[programIdx * 2u + 1u];
    uint codeOffset  = hdr0.x;
    uint codeLength  = hdr0.y;
    uint constOffset = hdr0.z;
    uint paramOffset = hdr1.x;

    float4 r[MAT_VM_MAX_REGS];
    for (uint i = 0u; i < uint(MAT_VM_MAX_REGS); ++i) r[i] = float4(0.0);

    for (uint pc = 0u; pc < codeLength; ++pc) {
        uint4 inst = code[codeOffset + pc];
        uint op  = inst.x & 0xFFFFu;
        uint dst = (inst.x >> 16) & 0xFFFFu;
        uint sA  = inst.y & 0xFFFFu;
        uint sB  = (inst.y >> 16) & 0xFFFFu;
        uint sC  = inst.z & 0xFFFFu;
        uint imm = (inst.z >> 16) & 0xFFFFu;

        switch (op) {
        case 0u:  pc = codeLength; break;                                   // Halt
        case 1u:  r[dst] = constantsBuf[constOffset + imm]; break;          // LoadConst
        case 2u:  r[dst] = float4(inp.worldPosition, 0.0); break;           // LoadPosition
        case 3u:  r[dst] = float4(inp.worldNormal, 0.0); break;             // LoadNormal
        case 4u:  r[dst] = float4(inp.worldTangent, 0.0); break;            // LoadTangent
        case 5u:  r[dst] = float4(inp.viewDir, 0.0); break;                 // LoadViewDir
        case 6u:  r[dst] = float4(inp.uv0, 0.0, 0.0); break;                // LoadUV0
        case 7u:  r[dst] = float4(inp.uv1, 0.0, 0.0); break;                // LoadUV1
        case 8u:  r[dst] = float4(inp.albedo, 0.0); break;                  // LoadInputAlbedo
        case 9u:  r[dst] = float4(inp.metallic); break;                     // LoadInputMetallic
        case 10u: r[dst] = float4(inp.roughness); break;                    // LoadInputRoughness
        case 11u: r[dst] = float4(inp.emission, 0.0); break;                // LoadInputEmission
        case 12u: r[dst] = float4(inp.normal, 0.0); break;                  // LoadInputNormal
        case 13u: r[dst] = r[sA] + r[sB]; break;                            // Add
        case 14u: r[dst] = r[sA] - r[sB]; break;                            // Sub
        case 15u: r[dst] = r[sA] * r[sB]; break;                            // Mul
        case 16u: r[dst] = r[sA] / r[sB]; break;                            // Div
        case 17u: r[dst] = -r[sA]; break;                                   // Neg
        case 18u: r[dst] = clamp(r[sA], float4(0.0), float4(1.0)); break;   // Saturate
        case 19u: r[dst] = mix(r[sA], r[sB], r[sC].x); break;               // Mix
        case 20u: r[dst] = clamp(r[sA], r[sB], r[sC]); break;               // Clamp
        case 21u: r[dst] = float4(dot(r[sA].xyz, r[sB].xyz)); break;        // Dot3
        case 22u: r[dst] = float4(length(r[sA].xyz)); break;                // Length3
        case 23u: r[dst] = float4(cross(r[sA].xyz, r[sB].xyz), 0.0); break; // Cross
        case 24u: r[dst] = float4(normalize(r[sA].xyz), 0.0); break;        // Normalize3
        case 25u: r[dst] = float4(r[sA].x); break;                          // Splat
        case 26u: res.albedo = r[sA].xyz; break;                            // WriteAlbedo
        case 27u: res.metallic = r[sA].x; break;                            // WriteMetallic
        case 28u: res.roughness = r[sA].x; break;                           // WriteRoughness
        case 29u: res.emission = r[sA].xyz; break;                          // WriteEmission
        case 30u: res.normal = r[sA].xyz; break;                            // WriteNormal
        case 31u: res.alpha = r[sA].x; break;                               // WriteAlpha
        case 32u: res.ior = r[sA].x; break;                                 // WriteIor
        case 33u: res.transmission = r[sA].x; break;                        // WriteTransmission
        case 34u: r[dst] = params[paramOffset + imm]; break;                // LoadParam
        case 35u: r[dst] = float4(float(inp.instanceIndex)); break;         // LoadInstanceIndex
        case 36u: r[dst] = float4(inp.transmission); break;                 // LoadInputTransmission
        case 37u: r[dst] = float4(inp.ior); break;                          // LoadInputIor
        case 38u: r[dst] = float4(inp.opacity); break;                      // LoadInputOpacity
        default: break;
        }
    }
    return res;
}

// ── Sky (sky_miss.glsl) ──────────────────────────────────────────────────

float3 sampleDomeGradient(device const float4 *lights, uint domeIdx, float3 dir) {
    float3 sky     = lights[domeIdx * 6u + 3u].xyz;
    float3 horizon = lights[domeIdx * 6u + 4u].xyz;
    float3 ground  = lights[domeIdx * 6u + 5u].xyz;
    float y = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    float3 upper = mix(horizon, sky,    smoothstep(0.5, 1.0, y));
    float3 lower = mix(horizon, ground, smoothstep(0.5, 0.0, y));
    return mix(lower, upper, smoothstep(0.45, 0.55, y));
}

float3 skyRadiance(device const float4 *lights, uint lightCount, float3 dir) {
    bool haveDome = false;
    uint domeIdx = 0u;
    for (uint li = 0u; li < lightCount; ++li) {
        if (int(lights[li * 6u + 0u].w) == 2) {  // LightType::Dome
            domeIdx = li;
            haveDome = true;
            break;
        }
    }
    if (haveDome) {
        float3 tint = lights[domeIdx * 6u + 2u].xyz * lights[domeIdx * 6u + 1u].w;
        return sampleDomeGradient(lights, domeIdx, dir) * tint;
    }
    float t = clamp(0.5 * (dir.y + 1.0), 0.0, 1.0);
    float3 horizon = float3(1.0, 0.55, 0.20);
    float3 zenith  = float3(0.15, 0.35, 1.00);
    return mix(horizon, zenith, t);
}

// ── Motion-blur intersect dispatch ─────────────────────────────────────────
// The static and motion paths use different intersector + acceleration-
// structure types (the instance_motion tag changes both, including the kernel's
// AS parameter type). These overloads pick the right intersect() per type so
// the shared megakernel body stays type-agnostic; the per-ray shutter `time`
// is ignored on the static path. MotionTraits maps an AS type to its
// intersector type + a compile-time motion flag.
struct HitFields {
    intersection_type type;
    uint instanceId;
    uint primId;
    float2 bary;
    float dist;
};

inline HitFields doIntersect(thread intersector<instancing, triangle_data> &isect,
                             instance_acceleration_structure accel, ray r, float) {
    auto h = isect.intersect(r, accel);
    return {h.type, h.instance_id, h.primitive_id, h.triangle_barycentric_coord, h.distance};
}
inline HitFields doIntersect(thread intersector<instancing, instance_motion, triangle_data> &isect,
                             acceleration_structure<instancing, instance_motion> accel, ray r, float time) {
    auto h = isect.intersect(r, accel, time);
    return {h.type, h.instance_id, h.primitive_id, h.triangle_barycentric_coord, h.distance};
}
inline bool doOccluded(thread intersector<instancing, triangle_data> &isect,
                       instance_acceleration_structure accel, ray r, float) {
    return isect.intersect(r, accel).type != intersection_type::none;
}
inline bool doOccluded(thread intersector<instancing, instance_motion, triangle_data> &isect,
                       acceleration_structure<instancing, instance_motion> accel, ray r, float time) {
    return isect.intersect(r, accel, time).type != intersection_type::none;
}

template <typename Accel> struct MotionTraits;
template <> struct MotionTraits<instance_acceleration_structure> {
    using Isect = intersector<instancing, triangle_data>;
    static constexpr constant bool motion = false;
};
template <> struct MotionTraits<acceleration_structure<instancing, instance_motion>> {
    using Isect = intersector<instancing, instance_motion, triangle_data>;
    static constexpr constant bool motion = true;
};

// ── Megakernel ───────────────────────────────────────────────────────────
// Shared body for both the static `pathtrace` and motion `pathtrace_motion`
// kernels (entry points below). Templated on the acceleration-structure type;
// the intersector type + motion flag follow from MotionTraits.
template <typename Accel>
void pathtraceImpl(
    texture2d<float, access::write> outImage,
    array<texture2d<float>, kMaxTextures> sceneTex,
    constant Uniforms &U,
    device const float4 *lights,
    device const int *materials,
    device const uint2 *instanceData,
    device const float2 *uvs,
    device const float4 *vertexNormals,
    device const packed_float3 *positions,
    device const float4 *normalMats,
    device const uint4 *progCode,
    device const float4 *progConst,
    device const uint4 *progHeaders,
    device const float4 *progParams,
    device float4 *accumBuffer,
    Accel accel,
    device const float4 *emitters,
    device float4 *aovAlbedo,
    device float4 *aovNormal,
    device float4 *aovDepth,
    device float4 *aovPosition,
    device float4 *aovEmission,
    device float4 *aovInstanceId,
    uint2 gid)
{
    constexpr bool kMotion = MotionTraits<Accel>::motion;
    if (gid.x >= U.width || gid.y >= U.height) return;
    const uint pixelIdx = gid.y * U.width + gid.x;

    float3 mean = accumBuffer[pixelIdx].xyz;

    // Running AOV means (parallel to `mean`), seeded from prior frames. Only
    // touched when enableAovs — the buffers are 1-element dummies otherwise.
    float4 aovAlbedoM = float4(0.0), aovNormalM = float4(0.0), aovDepthM = float4(0.0);
    float4 aovPositionM = float4(0.0), aovEmissionM = float4(0.0), aovIdM = float4(0.0);
    if (U.enableAovs) {
        aovAlbedoM = aovAlbedo[pixelIdx];     aovNormalM = aovNormal[pixelIdx];
        aovDepthM = aovDepth[pixelIdx];       aovPositionM = aovPosition[pixelIdx];
        aovEmissionM = aovEmission[pixelIdx]; aovIdM = aovInstanceId[pixelIdx];
    }

    const float width  = float(U.width);
    const float height = float(U.height);
    const float aspectRatio = width / height;
    const float tanHalfFov = tan((U.fovDegrees * PI / 180.0) / 2.0);

    typename MotionTraits<Accel>::Isect isect;
    isect.force_opacity(forced_opacity::opaque);
    isect.accept_any_intersection(false);

    for (uint s = 0u; s < U.samplesPerFrame; ++s) {
        // ── ray_gen.glsl ──
        uint globalSampleIdx = uint(U.currentSample - 1) * U.samplesPerFrame + s;
        uint seed = gid.x + gid.y * U.width + globalSampleIdx * U.width * U.height;
        float jitterX = hashSeed(seed);
        float jitterY = hashSeed(seed + 1u);

        float px = (2.0 * ((float(gid.x) + jitterX) / width) - 1.0) * tanHalfFov * aspectRatio;
        float py = (1.0 - 2.0 * ((float(gid.y) + jitterY) / height)) * tanHalfFov;

        ray r;
        r.origin = U.camPos;
        r.direction = normalize(U.camFwd + px * U.camRight + py * U.camUp);
        // Thin-lens depth of field: jitter the origin on the aperture disk and
        // re-aim through the focus point. Uses hashSeed (pure — does not touch
        // the nextRandom bounce stream), so aperture==0 is bit-identical.
        if (U.aperture > 0.0) {
            const float lr = U.aperture * sqrt(hashSeed(seed + 2u));
            const float lt = 2.0 * PI * hashSeed(seed + 3u);
            const float2 lens = float2(lr * cos(lt), lr * sin(lt));
            const float ft = U.focalDistance / dot(r.direction, U.camFwd);
            const float3 focus = r.origin + r.direction * ft;
            r.origin += lens.x * U.camRight + lens.y * U.camUp;
            r.direction = normalize(focus - r.origin);
        }
        r.min_distance = 0.01;
        r.max_distance = 1000.0;
        // Motion blur: per-sample shutter time in [0,1), passed to every
        // intersect of the path. hashSeed is pure → it never perturbs the
        // nextRandom bounce stream, and the static path ignores it entirely.
        const float sampleTime = kMotion ? hashSeed(seed + 4u) : 0.0;

        float3 color = float3(1.0);   // path throughput
        float3 accum = float3(0.0);   // emitted + NEE direct-light gathers
        // Per-sample AOV values, captured at the first shaded hit (or the
        // primary miss). Default = background (zero).
        float4 aAlbedo = float4(0.0), aNormal = float4(0.0), aDepth = float4(0.0);
        float4 aPos = float4(0.0), aEmis = float4(0.0), aId = float4(0.0);
        bool capturedPrimary = false;
        // Count a surface's own emission on direct arrival only when emitter
        // NEE couldn't have sampled it (camera ray / post-specular bounce).
        bool countEmissionOnHit = true;
        bool alive = true;

        for (uint depth = 0u; depth <= U.maxDepth && alive; ++depth) {
            HitFields hit = doIntersect(isect, accel, r, sampleTime);

            if (hit.type == intersection_type::none) {
                // ── sky_miss.glsl ── radiance accumulates into `accum`;
                // `color` is pure throughput.
                const float3 sky = skyRadiance(lights, U.lightCount, r.direction);
                // Primary miss: env colour is the background albedo guide.
                if (U.enableAovs && !capturedPrimary) {
                    aAlbedo = float4(sky, 1.0);
                    capturedPrimary = true;
                }
                accum += color * sky;
                alive = false;
                break;
            }

            // ── uber_hit.glsl ──
            if (depth >= U.maxDepth) {
                alive = false;
                break;
            }

            const uint instanceIdx = hit.instanceId;
            const uint triIdx = hit.primId;
            const float2 bary = hit.bary;
            const float u = bary.x;
            const float v = bary.y;
            const float w = 1.0 - u - v;
            const float3 hitPos = r.origin + r.direction * hit.dist;

            const uint base = instanceData[instanceIdx].y + triIdx * 3u;

            // Face normal: object-space cross of triangle edges, taken to
            // world space through the per-instance inverse-transpose
            // (matches the wavefront intersect's transpose(worldToObj)).
            const float3 p0 = float3(positions[base + 0u]);
            const float3 p1 = float3(positions[base + 1u]);
            const float3 p2 = float3(positions[base + 2u]);
            const float3 nObj = cross(p1 - p0, p2 - p0);
            const float3 nm0 = normalMats[instanceIdx * 3u + 0u].xyz;
            const float3 nm1 = normalMats[instanceIdx * 3u + 1u].xyz;
            const float3 nm2 = normalMats[instanceIdx * 3u + 2u].xyz;
            const float3 faceN = float3(dot(nm0, nObj), dot(nm1, nObj), dot(nm2, nObj));

            // getHitNormal: interpolated vertex normals (stored as-is — the
            // GLSL does not transform them either), face-normal fallback
            // when the slice is all-zero.
            const float3 n0 = vertexNormals[base + 0u].xyz;
            const float3 n1 = vertexNormals[base + 1u].xyz;
            const float3 n2 = vertexNormals[base + 2u].xyz;
            float3 N_raw;
            const float magSum = dot(n0, n0) + dot(n1, n1) + dot(n2, n2);
            if (magSum < 1e-6) {
                N_raw = normalize(faceN);
            } else {
                float3 n = w * n0 + u * n1 + v * n2;
                float len = length(n);
                N_raw = len > 1e-6 ? n / len : normalize(faceN);
            }

            const float3 incomingDir = normalize(r.direction);
            const float3 V = -incomingDir;
            const float NdotV_raw = dot(N_raw, V);
            const bool entering = NdotV_raw >= 0.0;
            // Robust shading normal (Schüssler 2017): when the interpolated
            // normal bends past the silhouette (N_raw·V<0) the old code flipped
            // it *inward* (N=-N_raw), killing sky GI / NEE → dark rim (badly
            // amplified by clearcoat). Instead reflect it back to the view
            // horizon — this keeps N·V>0 (no grazing specular spike) and keeps
            // the surface lit. Front-facing hits (the common case) are untouched.
            // Glass is two-sided and uses the raw normal flipped, locally below.
            const float3 N = (NdotV_raw < 0.0)
                                 ? normalize(N_raw - 2.0 * NdotV_raw * V)
                                 : N_raw;

            // getHitUV
            const float2 uv = w * uvs[base + 0u] + u * uvs[base + 1u] + v * uvs[base + 2u];

            const float3 hostAlbedo = getMaterialAlbedo(materials, sceneTex, instanceIdx, uv);
            // Emission scaled by the per-material strength (HDR emitters).
            const float3 hostEmission = getMaterialEmissive(materials, sceneTex, instanceIdx, uv)
                                        * getMaterialEmissiveStrength(materials, instanceIdx);
            const float2 hostMR = getMaterialMetallicRoughness(materials, sceneTex, instanceIdx, uv);

            float3 T, B;
            buildTangentFrame(N, T, B);

            MatInputs vmIn;
            vmIn.albedo = hostAlbedo;
            vmIn.metallic = hostMR.x;
            vmIn.roughness = hostMR.y;
            vmIn.emission = hostEmission;
            vmIn.normal = float3(0.0, 0.0, 1.0);
            vmIn.viewDir = V;
            vmIn.worldPosition = hitPos;
            vmIn.worldNormal = N;
            vmIn.worldTangent = T;
            vmIn.uv0 = uv;
            vmIn.uv1 = uv;
            vmIn.instanceIndex = instanceIdx;
            vmIn.transmission = getMaterialTransmission(materials, instanceIdx);
            vmIn.ior = getMaterialIor(materials, instanceIdx);
            vmIn.opacity = getMaterialOpacity(materials, instanceIdx);

            const uint programId = instanceData[instanceIdx].x;
            MatResult mat = runMaterialProgram(programId, vmIn,
                                               progCode, progConst, progHeaders, progParams);

            const float3 albedo = mat.albedo;
            const float3 emission = mat.emission;
            const float metallic = mat.metallic;
            const float roughness = clamp(mat.roughness, 0.04, 1.0);
            const float transmission = clamp(mat.transmission, 0.0, 1.0);
            const float ior = max(mat.ior, 1.0e-3);
            const float opacity = clamp(mat.alpha, 0.0, 1.0);
            const bool isGlass = transmission > 0.0 && metallic < 0.01;
            // R3 clear coat: a clear dielectric (F0=0.04) GGX layer over the base.
            const float clearcoat = clamp(getMaterialClearcoat(materials, instanceIdx), 0.0, 1.0);
            const float clearcoatRoughness = getMaterialClearcoatRoughness(materials, instanceIdx);
            // R3 sheen: grazing retroreflective term added to the diffuse NEE
            // (fabric/velvet). Purely additive (0 = off), so no RNG draw and no
            // change to existing renders.
            const float sheen = max(getMaterialSheen(materials, instanceIdx), 0.0);
            // R3d subsurface: wrap-diffusion weight + scatter tint. Blends the
            // diffuse NEE toward a softened, color-tinted response that wraps
            // light past the terminator (skin/wax). 0 = off (diffuse unchanged).
            const float subsurface = clamp(getMaterialSubsurface(materials, instanceIdx), 0.0, 1.0);
            const float3 subsurfaceColor = getMaterialSubsurfaceColor(materials, instanceIdx);
            // R3 anisotropy: [-1,1] stretches the metallic GGX highlight along
            // the UV tangent. Gated below (0 = isotropic; existing sampleGGX
            // path is untouched, so non-aniso materials render identically).
            const float anisotropy = clamp(getMaterialAnisotropy(materials, instanceIdx), -1.0, 1.0);
            // UV-aligned tangent frame for the anisotropic lobe (computed only
            // when needed; isotropic materials keep the arbitrary frame above).
            float3 Taniso = T, Baniso = B;
            if (anisotropy != 0.0) {
                Taniso = computeUVTangent(p0, p1, p2,
                                          uvs[base + 0u], uvs[base + 1u], uvs[base + 2u],
                                          N, T);
                Baniso = cross(N, Taniso);
            }

            // Stochastic opacity: with probability (1-opacity) the surface is
            // absent for this sample — pass the ray straight through (no shade,
            // no emit), preserving throughput. Counts as a bounce so stacks of
            // transparent surfaces stay bounded.
            if (opacity < 1.0 && nextRandom(seed) >= opacity) {
                r.origin = hitPos + incomingDir * 0.001;
                r.direction = incomingDir;
                continue;
            }

            // First shaded surface = primary visibility for AOVs.
            if (U.enableAovs && !capturedPrimary) {
                capturedPrimary = true;
                aAlbedo = float4(albedo, 1.0);
                aNormal = float4(N, 0.0);
                aDepth  = float4(length(hitPos - U.camPos), 0.0, 0.0, 0.0);
                aPos    = float4(hitPos, 1.0);
                aEmis   = float4(emission, 1.0);
                aId     = float4(float(instanceIdx + 1u), 0.0, 0.0, 0.0);
            }

            // Emitted radiance — counted on direct arrival only when emitter
            // NEE couldn't have sampled it (camera ray / post-specular), else
            // the NEE below handles it.
            if (countEmissionOnHit && length(emission) > 1e-6) {
                accum += color * emission;
            }

            // NEE — direct lighting with shadow rays. Dome is handled by the
            // miss shader; skipped for glass (pure delta lobes have no diffuse
            // term).
            if (U.lightCount > 0u && !isGlass) {
                const float3 diffuseBrdf = albedo * (1.0 - metallic) * (1.0 / 3.14159265);
                for (uint li = 0u; li < U.lightCount; ++li) {
                    const float4 posType    = lights[li * 6u + 0u];
                    const float4 dirIntens  = lights[li * 6u + 1u];
                    const float4 colorExtra = lights[li * 6u + 2u];

                    const int ltype = int(posType.w);
                    if (ltype == 2) continue;  // Dome

                    float3 Ldir;
                    float falloff;
                    float lightDist;  // shadow-ray reach
                    if (ltype == 0) {
                        const float3 toLight = posType.xyz - hitPos;
                        const float distSq = max(dot(toLight, toLight), 1e-4);
                        const float rad = colorExtra.w;
                        Ldir = toLight * rsqrt(distSq);
                        falloff = 1.0 / (distSq + rad * rad);
                        lightDist = sqrt(distSq);
                    } else if (ltype == 3) {
                        const float aw = colorExtra.w;
                        const float ah = lights[li * 6u + 3u].w;
                        Ldir = -normalize(dirIntens.xyz);
                        falloff = max(aw * ah, 1e-4);
                        lightDist = length(posType.xyz - hitPos);
                    } else {
                        Ldir = -normalize(dirIntens.xyz);
                        falloff = 1.0;
                        lightDist = 1.0e6;  // distant/sun: anything in the way occludes
                    }

                    // Subsurface wrap extends the lit band past the terminator
                    // by `subsurface`; at subsurface==0 this is the exact old
                    // `dot(N,Ldir) <= 0` early-out (parity-safe).
                    const float rawNdotL = dot(N, Ldir);
                    if (rawNdotL <= -subsurface) continue;
                    const float NdotLlight = max(rawNdotL, 0.0);

                    // Shadow ray: skip this light's contribution if blocked.
                    // Offset along N to avoid self-shadow acne; stop just short
                    // of the light so the light's own geometry doesn't occlude.
                    ray sray;
                    sray.origin = hitPos + N * 0.001;
                    sray.direction = Ldir;
                    sray.min_distance = 0.001;
                    sray.max_distance = max(lightDist - 0.002, 0.002);
                    if (doOccluded(isect, accel, sray, sampleTime)) continue;

                    const float3 Li = colorExtra.xyz * dirIntens.w * falloff;
                    const float3 Hs = normalize(Ldir + V);
                    const float sheenBrdf = sheen * pow(1.0 - max(dot(Ldir, Hs), 0.0), 5.0);
                    // Wrap-diffusion: blend Lambertian with a softened, tinted
                    // response. mix(...,0) == Lambertian, so subsurface==0 is a
                    // no-op and the diffuse NEE stays bit-identical.
                    const float wd = 1.0 + subsurface;
                    const float wrapCos = saturate((rawNdotL + subsurface) / (wd * wd));
                    const float3 sssResp = subsurfaceColor * (1.0 - metallic) * (1.0 / 3.14159265) * wrapCos;
                    const float3 diffuseLobe = mix(diffuseBrdf * NdotLlight, sssResp, subsurface);
                    accum += color * (diffuseLobe + sheenBrdf * NdotLlight) * Li;
                }
            }

            // Emissive area lights (NEE): sample one emissive triangle,
            // shadow-test it, add its contribution. Mirrors the CPU backend.
            if (U.emitterCount > 0u && !isGlass) {
                const float3 diffuseBrdf = albedo * (1.0 - metallic) * (1.0 / 3.14159265);
                const uint ne = U.emitterCount;
                const uint ei = min(uint(nextRandom(seed) * float(ne)), ne - 1u);
                const float4 e0 = emitters[ei * 4u + 0u]; // p0.xyz, area
                const float4 e1 = emitters[ei * 4u + 1u]; // p1
                const float4 e2 = emitters[ei * 4u + 2u]; // p2
                const float4 e3 = emitters[ei * 4u + 3u]; // emission
                const float su = sqrt(nextRandom(seed));
                const float b1 = 1.0 - su;
                const float b2 = nextRandom(seed) * su;
                const float3 y = e0.xyz + b1 * (e1.xyz - e0.xyz) + b2 * (e2.xyz - e0.xyz);
                const float3 toL = y - hitPos;
                const float dist2 = max(dot(toL, toL), 1e-6);
                const float dist = sqrt(dist2);
                const float3 wi = toL / dist;
                const float rawNdotL = dot(N, wi);
                float3 Ng = cross(e1.xyz - e0.xyz, e2.xyz - e0.xyz);
                const float ngLen = length(Ng);
                // subsurface wrap extends the band past the terminator; at
                // subsurface==0 this is the exact old `rawNdotL > 0` guard.
                if (rawNdotL > -subsurface && ngLen > 1e-12) {
                    Ng /= ngLen;
                    const float cosL = abs(dot(Ng, -wi));
                    if (cosL > 1e-4) {
                        ray sray;
                        sray.origin = hitPos + N * 0.001;
                        sray.direction = wi;
                        sray.min_distance = 0.001;
                        sray.max_distance = max(dist - 0.002, 0.002);
                        if (!doOccluded(isect, accel, sray, sampleTime)) {
                            const float w = e0.w * float(ne) * cosL / dist2; // area*ne*cosL/dist²
                            const float3 Hs = normalize(wi + V);
                            const float sheenBrdf = sheen * pow(1.0 - max(dot(wi, Hs), 0.0), 5.0);
                            const float NdotL = max(rawNdotL, 0.0);
                            const float wd = 1.0 + subsurface;
                            const float wrapCos = saturate((rawNdotL + subsurface) / (wd * wd));
                            const float3 sssResp = subsurfaceColor * (1.0 - metallic) * (1.0 / 3.14159265) * wrapCos;
                            const float3 diffuseLobe = mix(diffuseBrdf * NdotL, sssResp, subsurface);
                            accum += color * (diffuseLobe + sheenBrdf * NdotL) * e3.xyz * w;
                        }
                    }
                }
            }

            float r1 = nextRandom(seed);
            float r2 = nextRandom(seed);
            float r3 = nextRandom(seed);

            float3 L;
            float3 throughput;
            const float NdotV = max(dot(N, V), 0.001);

            // Clear coat decision (gated on clearcoat>0 so non-coat materials
            // draw no extra RNG and render identically). The coat is selected
            // with probability Fc = clearcoat·F_schlick(0.04); because this
            // integrator treats the selection probability as the blend weight
            // (no divide-by-pdf), that automatically attenuates the base layer
            // by (1-Fc) — energy-conserving.
            bool coatBounce = false;
            if (clearcoat > 0.0) {
                const float fc0 = 0.04 + 0.96 * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
                if (nextRandom(seed) < clearcoat * fc0) coatBounce = true;
            }

            if (coatBounce) {
                // White dielectric GGX coat at clearcoatRoughness.
                const float ccR = clamp(clearcoatRoughness, 0.04, 1.0);
                const float3 H_local = sampleGGX(r1, r2, ccR);
                float3 H = normalize(tangentToWorld(H_local, N, T, B));
                L = reflect(-V, H);
                float NdotL = dot(L, N);
                if (NdotL <= 0.0) { L = reflect(-V, N); NdotL = max(dot(L, N), 0.001); H = normalize(V + L); }
                NdotL = max(NdotL, 0.001);
                const float VdotH = max(dot(V, H), 0.001);
                const float NdotH = max(dot(N, H), 0.001);
                const float G = geometrySmith(NdotV, NdotL, ccR);
                throughput = float3(G * VdotH / (NdotV * NdotH));
            } else if (isGlass) {
                // Dielectric: Fresnel chooses reflect vs refract. The glass
                // tint is the surface baseColor applied to transmitted light
                // (glTF KHR_transmission "thin" model). Depth-based volumetric
                // absorption is a future add behind an attenuation param.
                // Glass is two-sided — orient the raw normal to the ray (the
                // old view-flipped normal; uses N_raw, not the bent shading N).
                const float3 gN = entering ? N_raw : -N_raw;
                const float etaI = entering ? 1.0 : ior;
                const float etaT = entering ? ior : 1.0;
                const float eta = etaI / etaT;
                const float cosI = clamp(dot(gN, V), 0.0, 1.0);
                const float F = fresnelDielectric(cosI, etaI, etaT);

                if (r3 < F) {
                    L = reflect(incomingDir, gN);
                    throughput = albedo;
                } else {
                    const float3 refracted = refract(incomingDir, gN, eta);
                    if (dot(refracted, refracted) < 1.0e-6) {
                        L = reflect(incomingDir, gN); // total internal reflection
                        throughput = albedo;
                    } else {
                        L = normalize(refracted);
                        const float etaScale = (etaT * etaT) / (etaI * etaI);
                        throughput = albedo * transmission * etaScale;
                    }
                }
            } else if (r3 < metallic) {
                // Anisotropy stretches the GGX highlight along the UV tangent.
                // Gated: anisotropy==0 takes the exact isotropic path (same
                // sample, same frame) so existing metal renders are unchanged.
                float3 H;
                if (anisotropy != 0.0) {
                    const float alpha = roughness * roughness;
                    const float aspect = sqrt(max(1.0 - 0.9 * abs(anisotropy), 1e-4));
                    float aT = max(alpha / aspect, 1e-4);
                    float aB = max(alpha * aspect, 1e-4);
                    if (anisotropy < 0.0) { float tmp = aT; aT = aB; aB = tmp; }
                    const float3 H_local = sampleGGXAniso(r1, r2, aT, aB);
                    H = normalize(tangentToWorld(H_local, N, Taniso, Baniso));
                } else {
                    const float3 H_local = sampleGGX(r1, r2, roughness);
                    H = normalize(tangentToWorld(H_local, N, T, B));
                }
                L = reflect(-V, H);

                float NdotL = dot(L, N);
                if (NdotL <= 0.0) {
                    L = reflect(-V, N);
                    NdotL = max(dot(L, N), 0.001);
                    H = normalize(V + L);
                }
                NdotL = max(NdotL, 0.001);

                const float VdotH = max(dot(V, H), 0.001);
                const float NdotH = max(dot(N, H), 0.001);

                const float3 F = fresnelSchlick(VdotH, albedo);
                const float G = geometrySmith(NdotV, NdotL, roughness);

                throughput = F * G * VdotH / (NdotV * NdotH);
            } else {
                const float3 L_local = sampleCosineHemisphere(r1, r2);
                L = normalize(tangentToWorld(L_local, N, T, B));
                throughput = albedo;
            }

            // Gate next-hit emission: diffuse bounces are covered by emitter
            // NEE above (don't double count); specular/glossy aren't, so they
            // must still count emitter arrivals. The coat is specular too.
            countEmissionOnHit = coatBounce || isGlass || (r3 < metallic);

            throughput = clamp(throughput, float3(0.0), float3(10.0));
            color *= throughput;

            const float3 offsetN = (dot(L, N) < 0.0) ? -N : N;
            r.origin = hitPos + offsetN * 0.001;
            r.direction = L;
        }

        // ── resolve.glsl: fold this sample into the running mean ──
        // All radiance (emission, sky, NEE) lives in `accum`; `color` is now
        // pure throughput, consumed during the walk.
        const float3 sampleColor = accum;
        const int n = (U.currentSample - 1) * int(U.samplesPerFrame) + int(s) + 1;
        mean = mean + (sampleColor - mean) / float(n);

        if (U.enableAovs) {
            const float fn = float(n);
            aovAlbedoM   += (aAlbedo - aovAlbedoM)   / fn;
            aovNormalM   += (aNormal - aovNormalM)   / fn;
            aovDepthM    += (aDepth  - aovDepthM)    / fn;
            aovPositionM += (aPos    - aovPositionM) / fn;
            aovEmissionM += (aEmis   - aovEmissionM) / fn;
            aovIdM       += (aId     - aovIdM)       / fn;
        }
    }

    accumBuffer[pixelIdx] = float4(mean, 1.0);
    if (U.enableAovs) {
        aovAlbedo[pixelIdx]     = aovAlbedoM;   aovNormal[pixelIdx]     = aovNormalM;
        aovDepth[pixelIdx]      = aovDepthM;    aovPosition[pixelIdx]   = aovPositionM;
        aovEmission[pixelIdx]   = aovEmissionM; aovInstanceId[pixelIdx] = aovIdM;
    }

    const float3 tonemapped = mean / (mean + float3(1.0));
    const float3 gammaCorrected = pow(tonemapped, float3(1.0 / 2.2));
    // Linear output (EXR/denoise) skips tonemap+gamma; display stays tonemapped.
    const float3 outRGB = U.linearOutput ? max(mean, float3(0.0)) : gammaCorrected;
    outImage.write(float4(outRGB, 1.0), gid);
}

// Static entry point (no motion blur) — bit-identical to the pre-R4 kernel.
kernel void pathtrace(
    texture2d<float, access::write> outImage          [[texture(0)]],
    array<texture2d<float>, kMaxTextures> sceneTex    [[texture(1)]],
    constant Uniforms &U                              [[buffer(0)]],
    device const float4 *lights                       [[buffer(1)]],
    device const int *materials                       [[buffer(2)]],
    device const uint2 *instanceData                  [[buffer(3)]],
    device const float2 *uvs                          [[buffer(4)]],
    device const float4 *vertexNormals                [[buffer(5)]],
    device const packed_float3 *positions             [[buffer(6)]],
    device const float4 *normalMats                   [[buffer(7)]],
    device const uint4 *progCode                      [[buffer(8)]],
    device const float4 *progConst                    [[buffer(9)]],
    device const uint4 *progHeaders                   [[buffer(10)]],
    device const float4 *progParams                   [[buffer(11)]],
    device float4 *accumBuffer                        [[buffer(12)]],
    instance_acceleration_structure accel             [[buffer(13)]],
    device const float4 *emitters                     [[buffer(14)]],
    device float4 *aovAlbedo                          [[buffer(15)]],
    device float4 *aovNormal                          [[buffer(16)]],
    device float4 *aovDepth                           [[buffer(17)]],
    device float4 *aovPosition                        [[buffer(18)]],
    device float4 *aovEmission                        [[buffer(19)]],
    device float4 *aovInstanceId                      [[buffer(20)]],
    uint2 gid                                         [[thread_position_in_grid]])
{
    pathtraceImpl(outImage, sceneTex, U, lights, materials, instanceData, uvs,
                  vertexNormals, positions, normalMats, progCode, progConst,
                  progHeaders, progParams, accumBuffer, accel, emitters, aovAlbedo,
                  aovNormal, aovDepth, aovPosition, aovEmission, aovInstanceId, gid);
}

// Motion-blur entry point: the AS parameter is a hardware motion AS, which
// selects the instance_motion intersector + the time-sampling intersect path.
kernel void pathtrace_motion(
    texture2d<float, access::write> outImage                    [[texture(0)]],
    array<texture2d<float>, kMaxTextures> sceneTex             [[texture(1)]],
    constant Uniforms &U                                       [[buffer(0)]],
    device const float4 *lights                                [[buffer(1)]],
    device const int *materials                                [[buffer(2)]],
    device const uint2 *instanceData                           [[buffer(3)]],
    device const float2 *uvs                                   [[buffer(4)]],
    device const float4 *vertexNormals                         [[buffer(5)]],
    device const packed_float3 *positions                      [[buffer(6)]],
    device const float4 *normalMats                            [[buffer(7)]],
    device const uint4 *progCode                               [[buffer(8)]],
    device const float4 *progConst                             [[buffer(9)]],
    device const uint4 *progHeaders                            [[buffer(10)]],
    device const float4 *progParams                            [[buffer(11)]],
    device float4 *accumBuffer                                 [[buffer(12)]],
    acceleration_structure<instancing, instance_motion> accel  [[buffer(13)]],
    device const float4 *emitters                              [[buffer(14)]],
    device float4 *aovAlbedo                                   [[buffer(15)]],
    device float4 *aovNormal                                   [[buffer(16)]],
    device float4 *aovDepth                                    [[buffer(17)]],
    device float4 *aovPosition                                 [[buffer(18)]],
    device float4 *aovEmission                                 [[buffer(19)]],
    device float4 *aovInstanceId                               [[buffer(20)]],
    uint2 gid                                                  [[thread_position_in_grid]])
{
    pathtraceImpl(outImage, sceneTex, U, lights, materials, instanceData, uvs,
                  vertexNormals, positions, normalMats, progCode, progConst,
                  progHeaders, progParams, accumBuffer, accel, emitters, aovAlbedo,
                  aovNormal, aovDepth, aovPosition, aovEmission, aovInstanceId, gid);
}

// Tonemap a LINEAR float4 buffer (e.g. the OIDN-denoised beauty) into the
// display texture, matching the per-sample resolve (Reinhard mean/(mean+1) +
// gamma 2.2). Used by the GPU backend's denoise() pass after OIDN writes the
// denoised linear result into `src`.
kernel void tonemap_buffer(
    texture2d<float, access::write> outImage [[texture(0)]],
    device const float4 *src                 [[buffer(0)]],
    constant uint2 &dims                      [[buffer(1)]],
    uint2 gid                                 [[thread_position_in_grid]])
{
    if (gid.x >= dims.x || gid.y >= dims.y) return;
    const float3 c = src[gid.y * dims.x + gid.x].rgb;
    const float3 t = c / (c + float3(1.0));
    const float3 g = pow(max(t, float3(0.0)), float3(1.0 / 2.2));
    outImage.write(float4(g, 1.0), gid);
}
)MSL";
} // namespace tracey
