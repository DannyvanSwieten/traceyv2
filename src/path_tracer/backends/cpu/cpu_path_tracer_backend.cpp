// See header. The per-pixel body below mirrors the Metal megakernel in
// ../metal/pathtrace_msl.hpp statement-for-statement (which itself ports
// the canonical GLSL set). When editing rendering semantics, change all
// three together — pt_backend_compare is the referee.

#include "cpu_path_tracer_backend.hpp"

#include "path_tracer/api/path_tracer.hpp"
#include "path_tracer/api/shader_inputs_view.hpp"
#include "core/parallel.hpp"
#include "shading/material_program/opcodes.hpp"

#include <glm/glm.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tracey
{
    namespace
    {
        constexpr float kPi = 3.14159265359f;

        // ── RNG (bit-exact: ray_gen.glsl hash / pbr_lib.glsl nextRandom) ──
        float hashSeed(uint32_t seed)
        {
            seed = (seed ^ 61u) ^ (seed >> 16u);
            seed *= 9u;
            seed = seed ^ (seed >> 4u);
            seed *= 0x27d4eb2du;
            seed = seed ^ (seed >> 15u);
            return static_cast<float>(seed) / 4294967296.0f;
        }

        float nextRandom(uint32_t &seed)
        {
            seed = (seed ^ 61u) ^ (seed >> 16u);
            seed *= 9u;
            seed = seed ^ (seed >> 4u);
            seed *= 0x27d4eb2du;
            seed = seed ^ (seed >> 15u);
            return static_cast<float>(seed) / 4294967296.0f;
        }

        // ── Sampling / fresnel / GGX (pbr_lib.glsl) ──
        glm::vec3 sampleCosineHemisphere(float r1, float r2)
        {
            const float phi = 2.0f * kPi * r1;
            const float cosTheta = std::sqrt(r2);
            const float sinTheta = std::sqrt(1.0f - r2);
            return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
        }

        glm::vec3 sampleGGX(float r1, float r2, float roughness)
        {
            const float a = roughness * roughness;
            const float a2 = std::max(a * a, 0.0001f);
            const float phi = 2.0f * kPi * r1;
            const float denom = std::max(1.0f + (a2 - 1.0f) * r2, 0.0001f);
            float cosTheta = std::sqrt((1.0f - r2) / denom);
            cosTheta = glm::clamp(cosTheta, 0.0f, 1.0f);
            const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
        }

        void buildTangentFrame(const glm::vec3 &N, glm::vec3 &T, glm::vec3 &B)
        {
            const glm::vec3 up = std::abs(N.y) < 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            T = glm::normalize(glm::cross(up, N));
            B = glm::cross(N, T);
        }

        glm::vec3 tangentToWorld(const glm::vec3 &v, const glm::vec3 &N,
                                 const glm::vec3 &T, const glm::vec3 &B)
        {
            return v.x * T + v.y * B + v.z * N;
        }

        glm::vec3 fresnelSchlick(float cosTheta, const glm::vec3 &F0)
        {
            return F0 + (glm::vec3(1.0f) - F0) * std::pow(std::max(1.0f - cosTheta, 0.0f), 5.0f);
        }

        float fresnelDielectric(float cosTheta, float etaI, float etaT)
        {
            float r0 = (etaI - etaT) / (etaI + etaT);
            r0 = r0 * r0;
            const float c = 1.0f - cosTheta;
            const float c2 = c * c;
            const float c5 = c2 * c2 * c;
            return r0 + (1.0f - r0) * c5;
        }

        float geometrySchlickGGX(float NdotV, float roughness)
        {
            const float r = roughness + 1.0f;
            const float k = (r * r) / 8.0f;
            const float denom = NdotV * (1.0f - k) + k;
            return NdotV / std::max(denom, 0.0001f);
        }

        float geometrySmith(float NdotV, float NdotL, float roughness)
        {
            return geometrySchlickGGX(NdotL, roughness) * geometrySchlickGGX(NdotV, roughness);
        }

        // ── Material fetch (pbr_lib.glsl over GPUMaterial) ──
        uint32_t samplerKindForSlot(const GPUMaterial &m, uint32_t slot)
        {
            return (m.samplerBits >> (slot * 2u)) & 0x3u;
        }

        glm::vec4 sampleTex(const std::vector<CpuTexture> &textures, int idx,
                            uint32_t kind, glm::vec2 uv)
        {
            if (idx < 0 || static_cast<size_t>(idx) >= textures.size()) return glm::vec4(1.0f);
            return textures[static_cast<size_t>(idx)].sample(uv, kind);
        }

        // ── Sky (sky_miss.glsl) ──
        glm::vec3 sampleDomeGradient(const std::vector<GPULight> &lights, uint32_t domeIdx,
                                     const glm::vec3 &dir)
        {
            const auto *slots = reinterpret_cast<const glm::vec4 *>(lights.data());
            const glm::vec3 sky = glm::vec3(slots[domeIdx * 6u + 3u]);
            const glm::vec3 horizon = glm::vec3(slots[domeIdx * 6u + 4u]);
            const glm::vec3 ground = glm::vec3(slots[domeIdx * 6u + 5u]);
            const float y = glm::clamp(dir.y * 0.5f + 0.5f, 0.0f, 1.0f);
            const glm::vec3 upper = glm::mix(horizon, sky, glm::smoothstep(0.5f, 1.0f, y));
            const glm::vec3 lower = glm::mix(horizon, ground, glm::smoothstep(0.5f, 0.0f, y));
            return glm::mix(lower, upper, glm::smoothstep(0.45f, 0.55f, y));
        }

        glm::vec3 skyRadiance(const std::vector<GPULight> &lights, uint32_t lightCount,
                              const glm::vec3 &dir)
        {
            const auto *slots = reinterpret_cast<const glm::vec4 *>(lights.data());
            bool haveDome = false;
            uint32_t domeIdx = 0;
            for (uint32_t li = 0; li < lightCount; ++li)
            {
                if (static_cast<int>(slots[li * 6u + 0u].w) == 2)
                {
                    domeIdx = li;
                    haveDome = true;
                    break;
                }
            }
            if (haveDome)
            {
                const glm::vec3 tint = glm::vec3(slots[domeIdx * 6u + 2u]) * slots[domeIdx * 6u + 1u].w;
                return sampleDomeGradient(lights, domeIdx, dir) * tint;
            }
            const float t = glm::clamp(0.5f * (dir.y + 1.0f), 0.0f, 1.0f);
            const glm::vec3 horizon(1.0f, 0.55f, 0.20f);
            const glm::vec3 zenith(0.15f, 0.35f, 1.00f);
            return glm::mix(horizon, zenith, t);
        }

        // ── Material VM over the packed buffer (material_vm.glsl + op 35) ──
        struct MatInputs
        {
            glm::vec3 albedo;
            float metallic;
            float roughness;
            glm::vec3 emission;
            glm::vec3 normal;
            glm::vec3 viewDir;
            glm::vec3 worldPosition;
            glm::vec3 worldNormal;
            glm::vec3 worldTangent;
            glm::vec2 uv0;
            glm::vec2 uv1;
            uint32_t instanceIndex;
            float transmission;
            float ior;
            float opacity;
        };

        struct MatResult
        {
            glm::vec3 albedo{0.5f};
            float metallic = 0.0f;
            float roughness = 0.5f;
            glm::vec3 emission{0.0f};
            glm::vec3 normal{0.0f, 0.0f, 1.0f};
            float alpha = 1.0f;
            float ior = 1.5f;
            float transmission = 0.0f;
        };

        MatResult runMaterialProgram(uint32_t programIdx, const MatInputs &inp,
                                     const MaterialProgramBuffer &programs)
        {
            MatResult res;
            if (programIdx >= programs.headers().size()) return res;
            const MaterialProgramBuffer::Header &hdr = programs.headers()[programIdx];

            glm::vec4 r[32];
            for (auto &reg : r) reg = glm::vec4(0.0f);

            const auto &code = programs.code();
            const auto &constants = programs.constants();
            const auto &params = programs.parameters();

            for (uint32_t pc = 0; pc < hdr.codeLength; ++pc)
            {
                const Instruction &inst = code[hdr.codeOffset + pc];
                const auto op = static_cast<Op>(inst.op);
                const uint16_t dst = inst.dst;
                const uint16_t sA = inst.srcA;
                const uint16_t sB = inst.srcB;
                const uint16_t sC = inst.srcC;
                const uint16_t imm = inst.imm;

                bool halt = false;
                switch (op)
                {
                case Op::Halt: halt = true; break;
                case Op::LoadConst:
                    r[dst] = reinterpret_cast<const glm::vec4 &>(constants[hdr.constOffset + imm]);
                    break;
                case Op::LoadPosition:      r[dst] = glm::vec4(inp.worldPosition, 0.0f); break;
                case Op::LoadNormal:        r[dst] = glm::vec4(inp.worldNormal, 0.0f); break;
                case Op::LoadTangent:       r[dst] = glm::vec4(inp.worldTangent, 0.0f); break;
                case Op::LoadViewDir:       r[dst] = glm::vec4(inp.viewDir, 0.0f); break;
                case Op::LoadUV0:           r[dst] = glm::vec4(inp.uv0, 0.0f, 0.0f); break;
                case Op::LoadUV1:           r[dst] = glm::vec4(inp.uv1, 0.0f, 0.0f); break;
                case Op::LoadInputAlbedo:   r[dst] = glm::vec4(inp.albedo, 0.0f); break;
                case Op::LoadInputMetallic: r[dst] = glm::vec4(inp.metallic); break;
                case Op::LoadInputRoughness:r[dst] = glm::vec4(inp.roughness); break;
                case Op::LoadInputEmission: r[dst] = glm::vec4(inp.emission, 0.0f); break;
                case Op::LoadInputNormal:   r[dst] = glm::vec4(inp.normal, 0.0f); break;
                case Op::Add: r[dst] = r[sA] + r[sB]; break;
                case Op::Sub: r[dst] = r[sA] - r[sB]; break;
                case Op::Mul: r[dst] = r[sA] * r[sB]; break;
                case Op::Div: r[dst] = r[sA] / r[sB]; break;
                case Op::Neg: r[dst] = -r[sA]; break;
                case Op::Saturate: r[dst] = glm::clamp(r[sA], glm::vec4(0.0f), glm::vec4(1.0f)); break;
                case Op::Mix: r[dst] = glm::mix(r[sA], r[sB], r[sC].x); break;
                case Op::Clamp: r[dst] = glm::clamp(r[sA], r[sB], r[sC]); break;
                case Op::Dot3: r[dst] = glm::vec4(glm::dot(glm::vec3(r[sA]), glm::vec3(r[sB]))); break;
                case Op::Length3: r[dst] = glm::vec4(glm::length(glm::vec3(r[sA]))); break;
                case Op::Cross: r[dst] = glm::vec4(glm::cross(glm::vec3(r[sA]), glm::vec3(r[sB])), 0.0f); break;
                case Op::Normalize3: r[dst] = glm::vec4(glm::normalize(glm::vec3(r[sA])), 0.0f); break;
                case Op::Splat: r[dst] = glm::vec4(r[sA].x); break;
                case Op::WriteAlbedo: res.albedo = glm::vec3(r[sA]); break;
                case Op::WriteMetallic: res.metallic = r[sA].x; break;
                case Op::WriteRoughness: res.roughness = r[sA].x; break;
                case Op::WriteEmission: res.emission = glm::vec3(r[sA]); break;
                case Op::WriteNormal: res.normal = glm::vec3(r[sA]); break;
                case Op::WriteAlpha: res.alpha = r[sA].x; break;
                case Op::WriteIor: res.ior = r[sA].x; break;
                case Op::WriteTransmission: res.transmission = r[sA].x; break;
                case Op::LoadParam:
                    r[dst] = reinterpret_cast<const glm::vec4 &>(params[hdr.paramOffset + imm]);
                    break;
                case Op::LoadInstanceIndex:
                    r[dst] = glm::vec4(static_cast<float>(inp.instanceIndex));
                    break;
                case Op::LoadInputTransmission: r[dst] = glm::vec4(inp.transmission); break;
                case Op::LoadInputIor:          r[dst] = glm::vec4(inp.ior); break;
                case Op::LoadInputOpacity:      r[dst] = glm::vec4(inp.opacity); break;
                default: break;
                }
                if (halt) break;
            }
            return res;
        }
    }

    void CpuPathTracerBackend::initialize(const InitParams &params)
    {
        m_config = params.config;
        m_shaderInputs = params.shaderInputs;
        if (!m_config) throw std::runtime_error("CpuPathTracerBackend: missing config");

        const size_t pixelCount = static_cast<size_t>(m_config->width) * m_config->height;
        m_accumulator.assign(pixelCount, glm::vec4(0.0f));
        const size_t pixelSize = m_config->hdrOutput ? 16 : 4;
        m_pixels.assign(pixelCount * pixelSize, 0);
    }

    void CpuPathTracerBackend::uploadMaterialPrograms(const MaterialProgramBuffer &programs)
    {
        m_programs = programs;
    }

    void CpuPathTracerBackend::uploadMaterialParameters(const MaterialProgramBuffer &programs)
    {
        m_programs.parameters() = programs.parameters();
    }

    void CpuPathTracerBackend::bindScene(const SceneCompiler::CompiledScene &scene)
    {
        if (scene.revision == m_sceneRevision) return;
        m_sceneRevision = scene.revision;

        // BVH: the engine's own CPU acceleration structures.
        m_blasPtrs.clear();
        for (const auto *blas : scene.blases)
        {
            const Blas *cpu = blas ? blas->cpuBlas() : nullptr;
            if (!cpu) throw std::runtime_error("CpuPathTracerBackend: BLAS without CPU data");
            m_blasPtrs.push_back(cpu);
        }
        m_instances = scene.instances;
        m_tlas = std::make_unique<Tlas>(
            std::span<const Blas *>(m_blasPtrs.data(), m_blasPtrs.size()),
            std::span<const Tlas::Instance>(m_instances.data(), m_instances.size()));

        m_lights = scene.lights;
        m_materials = scene.materials;

        const size_t instanceCount = scene.instances.size();
        m_instanceData.assign(std::max<size_t>(instanceCount, 1), glm::uvec2(0));
        for (size_t i = 0; i < instanceCount; ++i)
        {
            m_instanceData[i] = glm::uvec2(
                i < scene.instanceProgramIndex.size() ? scene.instanceProgramIndex[i] : 0u,
                i < scene.instanceUvOffset.size() ? scene.instanceUvOffset[i] : 0u);
        }

        uint32_t totalVertices = 0;
        for (uint32_t c : scene.vertexCounts) totalVertices += c;

        m_uvs.assign(std::max<uint32_t>(totalVertices, 1), glm::vec2(0.0f));
        if (scene.uvBuffer && totalVertices > 0)
        {
            std::memcpy(m_uvs.data(), scene.uvBuffer->mapForReading(),
                        static_cast<size_t>(totalVertices) * sizeof(glm::vec2));
            scene.uvBuffer->unmap();
        }

        m_normals.assign(std::max<uint32_t>(totalVertices, 1), glm::vec4(0.0f));
        if (scene.normalBuffer && totalVertices > 0)
        {
            std::memcpy(m_normals.data(), scene.normalBuffer->mapForReading(),
                        static_cast<size_t>(totalVertices) * sizeof(glm::vec4));
            scene.normalBuffer->unmap();
        }

        m_textures.clear();
        m_textures.reserve(scene.textureSources.size());
        for (const auto &src : scene.textureSources) m_textures.emplace_back(src);
    }

    double CpuPathTracerBackend::dispatch(const SceneCompiler::CompiledScene &scene,
                                          uint32_t /*accumulatedSampleCount*/,
                                          bool clearAccumulation,
                                          bool /*wantReadback*/)
    {
        const auto start = std::chrono::high_resolution_clock::now();

        bindScene(scene);
        if (clearAccumulation)
        {
            std::fill(m_accumulator.begin(), m_accumulator.end(), glm::vec4(0.0f));
        }

        const ShaderInputsView in = readShaderInputs(*m_shaderInputs);
        const uint32_t W = m_config->width;
        const uint32_t H = m_config->height;
        const uint32_t samplesPerFrame = m_config->samplesPerFrame;
        const float aspectRatio = static_cast<float>(W) / static_cast<float>(H);
        const float tanHalfFov = std::tan((in.fov * kPi / 180.0f) / 2.0f);

        parallel_for_chunks(static_cast<size_t>(W) * H, [&](size_t begin, size_t end) {
            for (size_t pixelIdx = begin; pixelIdx < end; ++pixelIdx)
            {
                const uint32_t px = static_cast<uint32_t>(pixelIdx % W);
                const uint32_t py = static_cast<uint32_t>(pixelIdx / W);

                glm::vec3 mean = glm::vec3(m_accumulator[pixelIdx]);

                for (uint32_t s = 0; s < samplesPerFrame; ++s)
                {
                    // ── ray_gen ──
                    const uint32_t globalSampleIdx =
                        static_cast<uint32_t>(in.currentSample - 1) * samplesPerFrame + s;
                    uint32_t seed = px + py * W + globalSampleIdx * W * H;
                    const float jitterX = hashSeed(seed);
                    const float jitterY = hashSeed(seed + 1u);

                    const float cx = (2.0f * ((static_cast<float>(px) + jitterX) / W) - 1.0f) *
                                     tanHalfFov * aspectRatio;
                    const float cy = (1.0f - 2.0f * ((static_cast<float>(py) + jitterY) / H)) *
                                     tanHalfFov;

                    Ray ray;
                    ray.origin = in.cameraPosition;
                    ray.direction = glm::normalize(in.cameraForward + cx * in.cameraRight +
                                                   cy * in.cameraUp);
                    ray.invDirection = glm::vec3(1.0f) / ray.direction;

                    glm::vec3 color(1.0f);
                    glm::vec3 accum(0.0f);
                    glm::vec3 medium(0.0f); // Beer-Lambert absorption of the
                                            // current medium (0 = vacuum)
                    bool alive = true;

                    for (uint32_t depth = 0; depth <= in.maxDepth && alive; ++depth)
                    {
                        const auto hit = m_tlas->intersect(ray, 0.01f, 1000.0f, RAY_FLAG_NONE);

                        if (!hit)
                        {
                            accum += color * skyRadiance(m_lights, in.lightCount, ray.direction);
                            alive = false;
                            break;
                        }

                        // Beer-Lambert: attenuate over the distance travelled in
                        // the current medium (no-op outside glass).
                        color *= glm::exp(-medium * hit->t);

                        // ── uber_hit ──
                        if (depth >= in.maxDepth)
                        {
                            alive = false;
                            break;
                        }

                        const uint32_t instanceIdx = hit->instanceId;
                        const uint32_t triIdx = hit->primitiveId;
                        const float u = hit->u;
                        const float v = hit->v;
                        const float w = 1.0f - u - v;
                        const glm::vec3 hitPos = hit->position;
                        const glm::vec3 faceN = hit->normal;

                        const uint32_t base = m_instanceData[instanceIdx].y + triIdx * 3u;

                        glm::vec3 N_raw;
                        const glm::vec3 n0 = glm::vec3(m_normals[base + 0u]);
                        const glm::vec3 n1 = glm::vec3(m_normals[base + 1u]);
                        const glm::vec3 n2 = glm::vec3(m_normals[base + 2u]);
                        const float magSum = glm::dot(n0, n0) + glm::dot(n1, n1) + glm::dot(n2, n2);
                        if (magSum < 1e-6f)
                        {
                            N_raw = glm::normalize(faceN);
                        }
                        else
                        {
                            const glm::vec3 n = w * n0 + u * n1 + v * n2;
                            const float len = glm::length(n);
                            N_raw = len > 1e-6f ? n / len : glm::normalize(faceN);
                        }

                        const glm::vec3 incomingDir = glm::normalize(ray.direction);
                        const glm::vec3 V = -incomingDir;
                        const bool entering = glm::dot(N_raw, V) >= 0.0f;
                        const glm::vec3 N = entering ? N_raw : -N_raw;

                        const glm::vec2 uv =
                            w * m_uvs[base + 0u] + u * m_uvs[base + 1u] + v * m_uvs[base + 2u];

                        const GPUMaterial &gm = m_materials[instanceIdx];
                        glm::vec3 hostAlbedo(gm.baseColorR, gm.baseColorG, gm.baseColorB);
                        if (gm.albedoTexIndex >= 0)
                        {
                            hostAlbedo *= glm::vec3(sampleTex(m_textures, gm.albedoTexIndex,
                                                              samplerKindForSlot(gm, 0u), uv));
                        }
                        glm::vec2 hostMR(gm.metallicFactor, gm.roughnessFactor);
                        if (gm.metallicRoughnessTexIndex >= 0)
                        {
                            const glm::vec4 mr = sampleTex(m_textures, gm.metallicRoughnessTexIndex,
                                                           samplerKindForSlot(gm, 2u), uv);
                            hostMR = glm::vec2(mr.b * gm.metallicFactor, mr.g * gm.roughnessFactor);
                        }
                        glm::vec3 hostEmission(gm.emissiveR, gm.emissiveG, gm.emissiveB);
                        if (gm.emissiveTexIndex >= 0)
                        {
                            hostEmission *= glm::vec3(sampleTex(m_textures, gm.emissiveTexIndex,
                                                                samplerKindForSlot(gm, 3u), uv));
                        }
                        hostEmission *= gm.emissiveStrength;

                        glm::vec3 T, B;
                        buildTangentFrame(N, T, B);

                        MatInputs vmIn;
                        vmIn.albedo = hostAlbedo;
                        vmIn.metallic = hostMR.x;
                        vmIn.roughness = hostMR.y;
                        vmIn.emission = hostEmission;
                        vmIn.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                        vmIn.viewDir = V;
                        vmIn.worldPosition = hitPos;
                        vmIn.worldNormal = N;
                        vmIn.worldTangent = T;
                        vmIn.uv0 = uv;
                        vmIn.uv1 = uv;
                        vmIn.instanceIndex = instanceIdx;
                        vmIn.transmission = gm.transmissionFactor;
                        vmIn.ior = gm.iorFactor;
                        vmIn.opacity = gm.baseColorA;

                        const MatResult mat =
                            runMaterialProgram(m_instanceData[instanceIdx].x, vmIn, m_programs);

                        const glm::vec3 albedo = mat.albedo;
                        const glm::vec3 emission = mat.emission;
                        const float metallic = mat.metallic;
                        const float roughness = glm::clamp(mat.roughness, 0.04f, 1.0f);
                        const float transmission = glm::clamp(mat.transmission, 0.0f, 1.0f);
                        const float ior = std::max(mat.ior, 1.0e-3f);
                        const float opacity = glm::clamp(mat.alpha, 0.0f, 1.0f);
                        const bool isGlass = transmission > 0.0f && metallic < 0.01f;

                        // Stochastic opacity: with prob (1-opacity) the surface
                        // is absent for this sample — pass the ray straight
                        // through (no shade, no emit), preserving throughput.
                        if (opacity < 1.0f && nextRandom(seed) >= opacity)
                        {
                            ray.origin = hitPos + incomingDir * 0.001f;
                            ray.direction = incomingDir;
                            ray.invDirection = glm::vec3(1.0f) / ray.direction;
                            continue;
                        }

                        // Emission is additive (an emitter glows AND lights the
                        // scene via bounces). misWeight 1 here; M3 adds MIS.
                        if (glm::length(emission) > 0.0f)
                        {
                            accum += color * emission;
                        }

                        if (in.lightCount > 0 && !isGlass)
                        {
                            const auto *slots = reinterpret_cast<const glm::vec4 *>(m_lights.data());
                            const glm::vec3 diffuseBrdf =
                                albedo * (1.0f - metallic) * (1.0f / 3.14159265f);
                            for (uint32_t li = 0; li < in.lightCount; ++li)
                            {
                                const glm::vec4 posType = slots[li * 6u + 0u];
                                const glm::vec4 dirIntens = slots[li * 6u + 1u];
                                const glm::vec4 colorExtra = slots[li * 6u + 2u];

                                const int ltype = static_cast<int>(posType.w);
                                if (ltype == 2) continue;  // Dome

                                glm::vec3 Ldir;
                                float falloff;
                                if (ltype == 0)
                                {
                                    const glm::vec3 toLight = glm::vec3(posType) - hitPos;
                                    const float distSq = std::max(glm::dot(toLight, toLight), 1e-4f);
                                    const float rad = colorExtra.w;
                                    Ldir = toLight * (1.0f / std::sqrt(distSq));
                                    falloff = 1.0f / (distSq + rad * rad);
                                }
                                else if (ltype == 3)
                                {
                                    const float aw = colorExtra.w;
                                    const float ah = slots[li * 6u + 3u].w;
                                    Ldir = -glm::normalize(glm::vec3(dirIntens));
                                    falloff = std::max(aw * ah, 1e-4f);
                                }
                                else
                                {
                                    Ldir = -glm::normalize(glm::vec3(dirIntens));
                                    falloff = 1.0f;
                                }

                                const float NdotLlight = std::max(glm::dot(N, Ldir), 0.0f);
                                if (NdotLlight <= 0.0f) continue;

                                const glm::vec3 Li = glm::vec3(colorExtra) * dirIntens.w * falloff;
                                accum += color * diffuseBrdf * Li * NdotLlight;
                            }
                        }

                        const float r1 = nextRandom(seed);
                        const float r2 = nextRandom(seed);
                        const float r3 = nextRandom(seed);

                        glm::vec3 L;
                        glm::vec3 throughput;
                        const float NdotV = std::max(glm::dot(N, V), 0.001f);

                        if (isGlass)
                        {
                            // Dielectric: reflection is colorless; refraction
                            // tint comes from Beer-Lambert absorption inside.
                            const float etaI = entering ? 1.0f : ior;
                            const float etaT = entering ? ior : 1.0f;
                            const float eta = etaI / etaT;
                            const float cosI = glm::clamp(glm::dot(N, V), 0.0f, 1.0f);
                            const float F = fresnelDielectric(cosI, etaI, etaT);

                            if (r3 < F)
                            {
                                L = glm::reflect(incomingDir, N);
                                throughput = glm::vec3(1.0f);
                            }
                            else
                            {
                                const glm::vec3 refracted = glm::refract(incomingDir, N, eta);
                                if (glm::dot(refracted, refracted) < 1.0e-6f)
                                {
                                    L = glm::reflect(incomingDir, N); // total internal reflection
                                    throughput = glm::vec3(1.0f);
                                }
                                else
                                {
                                    L = glm::normalize(refracted);
                                    const float etaScale = (etaT * etaT) / (etaI * etaI);
                                    throughput = glm::vec3(transmission * etaScale);
                                    medium = entering
                                        ? -glm::log(glm::clamp(albedo, glm::vec3(1.0e-3f), glm::vec3(1.0f)))
                                        : glm::vec3(0.0f);
                                }
                            }
                        }
                        else if (r3 < metallic)
                        {
                            const glm::vec3 H_local = sampleGGX(r1, r2, roughness);
                            glm::vec3 Hv = glm::normalize(tangentToWorld(H_local, N, T, B));
                            L = glm::reflect(-V, Hv);

                            float NdotL = glm::dot(L, N);
                            if (NdotL <= 0.0f)
                            {
                                L = glm::reflect(-V, N);
                                NdotL = std::max(glm::dot(L, N), 0.001f);
                                Hv = glm::normalize(V + L);
                            }
                            NdotL = std::max(NdotL, 0.001f);

                            const float VdotH = std::max(glm::dot(V, Hv), 0.001f);
                            const float NdotH = std::max(glm::dot(N, Hv), 0.001f);

                            const glm::vec3 F = fresnelSchlick(VdotH, albedo);
                            const float G = geometrySmith(NdotV, NdotL, roughness);

                            throughput = F * G * VdotH / (NdotV * NdotH);
                        }
                        else
                        {
                            const glm::vec3 L_local = sampleCosineHemisphere(r1, r2);
                            L = glm::normalize(tangentToWorld(L_local, N, T, B));
                            throughput = albedo;
                        }

                        throughput = glm::clamp(throughput, glm::vec3(0.0f), glm::vec3(10.0f));
                        color *= throughput;

                        const glm::vec3 offsetN = (glm::dot(L, N) < 0.0f) ? -N : N;
                        ray.origin = hitPos + offsetN * 0.001f;
                        ray.direction = L;
                        ray.invDirection = glm::vec3(1.0f) / ray.direction;
                    }

                    // ── resolve ──
                    // All radiance (emission, sky, NEE) lives in `accum`;
                    // `color` is pure throughput, consumed during the walk.
                    const glm::vec3 sampleColor = accum;
                    const int n = (in.currentSample - 1) * static_cast<int>(samplesPerFrame) +
                                  static_cast<int>(s) + 1;
                    mean = mean + (sampleColor - mean) / static_cast<float>(n);
                }

                m_accumulator[pixelIdx] = glm::vec4(mean, 1.0f);

                const glm::vec3 tonemapped = mean / (mean + glm::vec3(1.0f));
                const glm::vec3 gammaCorrected =
                    glm::pow(tonemapped, glm::vec3(1.0f / 2.2f));

                if (m_config->hdrOutput)
                {
                    auto *out = reinterpret_cast<float *>(m_pixels.data()) + pixelIdx * 4;
                    out[0] = gammaCorrected.r;
                    out[1] = gammaCorrected.g;
                    out[2] = gammaCorrected.b;
                    out[3] = 1.0f;
                }
                else
                {
                    auto *out = m_pixels.data() + pixelIdx * 4;
                    out[0] = static_cast<uint8_t>(glm::clamp(gammaCorrected.r, 0.0f, 1.0f) * 255.0f + 0.5f);
                    out[1] = static_cast<uint8_t>(glm::clamp(gammaCorrected.g, 0.0f, 1.0f) * 255.0f + 0.5f);
                    out[2] = static_cast<uint8_t>(glm::clamp(gammaCorrected.b, 0.0f, 1.0f) * 255.0f + 0.5f);
                    out[3] = 255;
                }
            }
        });

        const auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    size_t CpuPathTracerBackend::readback(void *dst)
    {
        std::memcpy(dst, m_pixels.data(), m_pixels.size());
        return m_pixels.size();
    }
} // namespace tracey
