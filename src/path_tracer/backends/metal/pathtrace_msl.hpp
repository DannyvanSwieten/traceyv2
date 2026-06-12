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

void buildTangentFrame(float3 N, thread float3 &T, thread float3 &B) {
    float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
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

// ── Material fetch (pbr_lib.glsl, GPUMaterial as int[20]) ────────────────

constant uint MATERIAL_STRIDE = 20;

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

// ── Megakernel ───────────────────────────────────────────────────────────

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
    uint2 gid                                         [[thread_position_in_grid]])
{
    if (gid.x >= U.width || gid.y >= U.height) return;
    const uint pixelIdx = gid.y * U.width + gid.x;

    float3 mean = accumBuffer[pixelIdx].xyz;

    const float width  = float(U.width);
    const float height = float(U.height);
    const float aspectRatio = width / height;
    const float tanHalfFov = tan((U.fovDegrees * PI / 180.0) / 2.0);

    intersector<instancing, triangle_data> isect;
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
        r.min_distance = 0.01;
        r.max_distance = 1000.0;

        float3 color = float3(1.0);   // path throughput
        float3 accum = float3(0.0);   // NEE direct-light gathers
        bool alive = true;

        for (uint depth = 0u; depth <= U.maxDepth && alive; ++depth) {
            intersection_result<instancing, triangle_data> hit = isect.intersect(r, accel);

            if (hit.type == intersection_type::none) {
                // ── sky_miss.glsl ──
                color *= skyRadiance(lights, U.lightCount, r.direction);
                alive = false;
                break;
            }

            // ── uber_hit.glsl ──
            if (depth >= U.maxDepth) {
                color = float3(0.0);
                alive = false;
                break;
            }

            const uint instanceIdx = hit.instance_id;
            const uint triIdx = hit.primitive_id;
            const float2 bary = hit.triangle_barycentric_coord;
            const float u = bary.x;
            const float v = bary.y;
            const float w = 1.0 - u - v;
            const float3 hitPos = r.origin + r.direction * hit.distance;

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
            const bool entering = dot(N_raw, V) >= 0.0;
            const float3 N = entering ? N_raw : -N_raw;

            // getHitUV
            const float2 uv = w * uvs[base + 0u] + u * uvs[base + 1u] + v * uvs[base + 2u];

            const float3 hostAlbedo = getMaterialAlbedo(materials, sceneTex, instanceIdx, uv);
            const float3 hostEmission = getMaterialEmissive(materials, sceneTex, instanceIdx, uv);
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

            const uint programId = instanceData[instanceIdx].x;
            MatResult mat = runMaterialProgram(programId, vmIn,
                                               progCode, progConst, progHeaders, progParams);

            const float3 albedo = mat.albedo;
            const float3 emission = mat.emission;
            const float metallic = mat.metallic;
            const float roughness = clamp(mat.roughness, 0.04, 1.0);
            const float transmission = clamp(mat.transmission, 0.0, 1.0);
            const float ior = max(mat.ior, 1.0e-3);
            const bool isGlass = transmission > 0.0 && metallic < 0.01;

            if (length(emission) > 0.0) {
                color *= emission;
                alive = false;
                break;
            }

            // NEE — unshadowed; dome handled by the miss shader; skipped for
            // glass (pure delta lobes have no diffuse term).
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
                    if (ltype == 0) {
                        const float3 toLight = posType.xyz - hitPos;
                        const float distSq = max(dot(toLight, toLight), 1e-4);
                        const float rad = colorExtra.w;
                        Ldir = toLight * rsqrt(distSq);
                        falloff = 1.0 / (distSq + rad * rad);
                    } else if (ltype == 3) {
                        const float aw = colorExtra.w;
                        const float ah = lights[li * 6u + 3u].w;
                        Ldir = -normalize(dirIntens.xyz);
                        falloff = max(aw * ah, 1e-4);
                    } else {
                        Ldir = -normalize(dirIntens.xyz);
                        falloff = 1.0;
                    }

                    const float NdotLlight = max(dot(N, Ldir), 0.0);
                    if (NdotLlight <= 0.0) continue;

                    const float3 Li = colorExtra.xyz * dirIntens.w * falloff;
                    accum += color * diffuseBrdf * Li * NdotLlight;
                }
            }

            float r1 = nextRandom(seed);
            float r2 = nextRandom(seed);
            float r3 = nextRandom(seed);

            float3 L;
            float3 throughput;
            const float NdotV = max(dot(N, V), 0.001);

            if (isGlass) {
                const float etaI = entering ? 1.0 : ior;
                const float etaT = entering ? ior : 1.0;
                const float eta = etaI / etaT;
                const float cosI = clamp(dot(N, V), 0.0, 1.0);
                const float F = fresnelDielectric(cosI, etaI, etaT);

                if (r3 < F) {
                    L = reflect(incomingDir, N);
                    throughput = albedo;
                } else {
                    const float3 refracted = refract(incomingDir, N, eta);
                    if (dot(refracted, refracted) < 1.0e-6) {
                        L = reflect(incomingDir, N);
                        throughput = albedo;
                    } else {
                        L = normalize(refracted);
                        const float etaScale = (etaT * etaT) / (etaI * etaI);
                        throughput = albedo * transmission * etaScale;
                    }
                }
            } else if (r3 < metallic) {
                const float3 H_local = sampleGGX(r1, r2, roughness);
                float3 H = normalize(tangentToWorld(H_local, N, T, B));
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

            throughput = clamp(throughput, float3(0.0), float3(10.0));
            color *= throughput;

            const float3 offsetN = (dot(L, N) < 0.0) ? -N : N;
            r.origin = hitPos + offsetN * 0.001;
            r.direction = L;
        }

        // ── resolve.glsl: fold this sample into the running mean ──
        const float3 sampleColor = color + accum;
        const int n = (U.currentSample - 1) * int(U.samplesPerFrame) + int(s) + 1;
        mean = mean + (sampleColor - mean) / float(n);
    }

    accumBuffer[pixelIdx] = float4(mean, 1.0);

    const float3 tonemapped = mean / (mean + float3(1.0));
    const float3 gammaCorrected = pow(tonemapped, float3(1.0 / 2.2));
    outImage.write(float4(gammaCorrected, 1.0), gid);
}
)MSL";
} // namespace tracey
