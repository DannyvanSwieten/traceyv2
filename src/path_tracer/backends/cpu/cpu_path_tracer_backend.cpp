// See header. The per-pixel body below mirrors the Metal megakernel in
// ../metal/pathtrace_msl.hpp statement-for-statement (which itself ports
// the canonical GLSL set). When editing rendering semantics, change all
// three together — pt_backend_compare is the referee.

#include "cpu_path_tracer_backend.hpp"

#include "path_tracer/api/path_tracer.hpp"
#include "path_tracer/api/shader_inputs_view.hpp"
#include "core/parallel.hpp"
#include "io/denoiser.hpp"   // interactive denoise post-pass (OIDN, guarded)
#include "shading/material_program/opcodes.hpp"

#include <glm/glm.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
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

        // Anisotropic GGX half-vector sample in tangent space (x=T, y=B, z=N);
        // reduces exactly to sampleGGX at aT==aB. Mirrors the MSL backend.
        glm::vec3 sampleGGXAniso(float r1, float r2, float aT, float aB)
        {
            const float phi = std::atan2(aB * std::sin(2.0f * kPi * r1),
                                         aT * std::cos(2.0f * kPi * r1));
            const float cosPhi = std::cos(phi), sinPhi = std::sin(phi);
            const float A = (cosPhi * cosPhi) / std::max(aT * aT, 1e-8f) +
                            (sinPhi * sinPhi) / std::max(aB * aB, 1e-8f);
            const float tanTheta2 = r2 / std::max((1.0f - r2) * A, 1e-8f);
            const float cosTheta = 1.0f / std::sqrt(1.0f + tanTheta2);
            const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            return {cosPhi * sinTheta, sinPhi * sinTheta, cosTheta};
        }

        void buildTangentFrame(const glm::vec3 &N, glm::vec3 &T, glm::vec3 &B)
        {
            const glm::vec3 up = std::abs(N.y) < 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            T = glm::normalize(glm::cross(up, N));
            B = glm::cross(N, T);
        }

        // UV-aligned tangent (Lengyel), Gram-Schmidt against N; falls back to
        // `fallbackT` for degenerate UVs. Mirrors the MSL backend.
        glm::vec3 computeUVTangent(const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &p2,
                                   const glm::vec2 &uv0, const glm::vec2 &uv1, const glm::vec2 &uv2,
                                   const glm::vec3 &N, const glm::vec3 &fallbackT)
        {
            const glm::vec3 e1 = p1 - p0, e2 = p2 - p0;
            const glm::vec2 d1 = uv1 - uv0, d2 = uv2 - uv0;
            const float det = d1.x * d2.y - d2.x * d1.y;
            if (std::abs(det) < 1e-12f) return fallbackT;
            glm::vec3 Traw = e1 * d2.y - e2 * d1.y;
            Traw = Traw - N * glm::dot(N, Traw);
            const float l = glm::length(Traw);
            return l > 1e-8f ? Traw / l : fallbackT;
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
            // No Dome light in the scene → no environment radiance. Deleting the
            // Dome light therefore truly disables environment lighting AND the sky
            // background. (Previously this returned a hardcoded gradient, so the
            // scene kept being lit and "the dome kept rendering" after removal.)
            // Add a Dome light back to get a sky. Mirrored in the Metal backend.
            return glm::vec3(0.0f);
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
        if (m_config->enableAovs)
            for (auto &a : m_aovs) a.assign(pixelCount, glm::vec4(0.0f));
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
        // Already bound to this scene AND the TLAS actually got built. The m_tlas
        // check is essential: a prior bind that bailed before building the TLAS
        // (scene not yet built for CPU tracing) must NOT count as "bound", or the
        // trace loop would dereference a null m_tlas. m_sceneRevision is committed
        // only on full success (end of this function), so this stays honest.
        if (scene.revision == m_sceneRevision && m_tlas) return;

        // BVH: the engine's own CPU acceleration structures. A scene compiled in
        // raster-only mode (buildAccelerationStructures == false) leaves the BLAS
        // pointers null — it isn't renderable on the CPU backend. Bail WITHOUT
        // committing m_sceneRevision (so a later, complete compile rebinds) and
        // WITHOUT touching m_tlas; dispatch() skips tracing when m_tlas is null,
        // so this can never crash. Build into a local first so a mid-loop bail
        // doesn't leave m_blasPtrs half-populated.
        std::vector<const Blas *> blasPtrs;
        blasPtrs.reserve(scene.blases.size());
        for (const auto *blas : scene.blases)
        {
            const Blas *cpu = blas ? blas->cpuBlas() : nullptr;
            if (!cpu)
            {
                std::fprintf(stderr, "[cpu-pt] bindScene: a BLAS has no CPU data — "
                                     "scene not built for CPU tracing; skipping bind\n");
                return;
            }
            blasPtrs.push_back(cpu);
        }
        m_blasPtrs = std::move(blasPtrs);
        m_instances = scene.instances;
        m_instancesEnd = scene.instancesEnd;
        m_hasMotion = scene.hasMotion && m_instancesEnd.size() == m_instances.size();
        m_tlas = std::make_unique<Tlas>(
            std::span<const Blas *>(m_blasPtrs.data(), m_blasPtrs.size()),
            std::span<const Tlas::Instance>(m_instances.data(), m_instances.size()),
            std::span<const Tlas::Instance>(m_instancesEnd.data(), m_instancesEnd.size()),
            m_hasMotion, Tlas::Config{});

        m_lights = scene.lights;
        m_emitters = scene.emitters;
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

        // Object-space positions, parallel to m_uvs/m_normals — used to derive
        // UV-aligned tangents at the hit for the anisotropic GGX lobe.
        m_positions.assign(std::max<uint32_t>(totalVertices, 1), glm::vec4(0.0f));
        if (scene.positionBuffer && totalVertices > 0)
        {
            std::memcpy(m_positions.data(), scene.positionBuffer->mapForReading(),
                        static_cast<size_t>(totalVertices) * sizeof(glm::vec4));
            scene.positionBuffer->unmap();
        }

        m_textures.clear();
        m_textures.reserve(scene.textureSources.size());
        for (const auto &src : scene.textureSources) m_textures.emplace_back(src);

        // Commit only now that the bind fully succeeded (TLAS built, buffers
        // copied). Stamping it up front — as this used to — meant an early bail or
        // throw left the scene marked "bound" with m_tlas null, and the next
        // dispatch early-returned straight into a null dereference.
        m_sceneRevision = scene.revision;
    }

    double CpuPathTracerBackend::dispatch(const SceneCompiler::CompiledScene &scene,
                                          uint32_t /*accumulatedSampleCount*/,
                                          bool clearAccumulation,
                                          bool /*wantReadback*/)
    {
        const auto start = std::chrono::high_resolution_clock::now();

        bindScene(scene);
        // bindScene leaves m_tlas null when the scene isn't built for CPU tracing
        // (raster-only compile). Skip the trace rather than dereference null in the
        // intersect loop — the frame stays as-is (cleared/empty) and no work is done.
        if (!m_tlas)
            return 0.0;

        const uint32_t W = m_config->width;
        const uint32_t H = m_config->height;
        const size_t pixelCount = static_cast<size_t>(W) * H;

        // AOV layers track the beauty accumulator: allocate lazily (config may
        // have flipped enableAovs after initialize) and clear in lockstep.
        const bool aovs = m_config->enableAovs;
        if (aovs && m_aovs[0].size() != pixelCount)
            for (auto &a : m_aovs) a.assign(pixelCount, glm::vec4(0.0f));

        if (clearAccumulation)
        {
            std::fill(m_accumulator.begin(), m_accumulator.end(), glm::vec4(0.0f));
            if (aovs)
                for (auto &a : m_aovs) std::fill(a.begin(), a.end(), glm::vec4(0.0f));
        }

        const ShaderInputsView in = readShaderInputs(*m_shaderInputs);
        const uint32_t samplesPerFrame = m_config->samplesPerFrame;
        const float aspectRatio = static_cast<float>(W) / static_cast<float>(H);
        const float tanHalfFov = std::tan((in.fov * kPi / 180.0f) / 2.0f);

        parallel_for_chunks(static_cast<size_t>(W) * H, [&](size_t begin, size_t end) {
            for (size_t pixelIdx = begin; pixelIdx < end; ++pixelIdx)
            {
                const uint32_t px = static_cast<uint32_t>(pixelIdx % W);
                const uint32_t py = static_cast<uint32_t>(pixelIdx / W);

                glm::vec3 mean = glm::vec3(m_accumulator[pixelIdx]);

                // Running AOV means (parallel to `mean`), seeded from prior frames.
                constexpr size_t kAovN = static_cast<size_t>(AovKind::Count);
                glm::vec4 aovMean[kAovN];
                if (aovs)
                    for (size_t k = 0; k < kAovN; ++k) aovMean[k] = m_aovs[k][pixelIdx];

                for (uint32_t s = 0; s < samplesPerFrame; ++s)
                {
                    // Per-sample AOV values, captured at the first shaded hit
                    // (or the primary miss). Default = background (zero).
                    glm::vec4 aovSample[kAovN] = {};
                    bool capturedPrimary = false;
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
                    // Thin-lens DOF (mirrors MSL; hashSeed is pure so aperture==0
                    // is bit-identical and never disturbs the nextRandom stream).
                    if (in.aperture > 0.0f)
                    {
                        const float lr = in.aperture * std::sqrt(hashSeed(seed + 2u));
                        const float lt = 2.0f * kPi * hashSeed(seed + 3u);
                        const glm::vec2 lens(lr * std::cos(lt), lr * std::sin(lt));
                        const float ft =
                            in.focalDistance / glm::dot(ray.direction, in.cameraForward);
                        const glm::vec3 focus = ray.origin + ray.direction * ft;
                        ray.origin += lens.x * in.cameraRight + lens.y * in.cameraUp;
                        ray.direction = glm::normalize(focus - ray.origin);
                    }
                    ray.invDirection = glm::vec3(1.0f) / ray.direction;
                    // Motion blur: a per-sample shutter time in [0,1). The TLAS
                    // interpolates instance poses by this; carried on every ray
                    // of the path (incl. shadow rays) for a consistent shutter
                    // instant. hashSeed is pure → no perturbation when static.
                    const float sampleTime = m_hasMotion ? hashSeed(seed + 4u) : 0.0f;
                    ray.time = sampleTime;

                    glm::vec3 color(1.0f);
                    glm::vec3 accum(0.0f);
                    // Count a surface's own emission on direct arrival only when
                    // NEE couldn't have sampled it: the camera ray and rays that
                    // arrive via a specular/glossy bounce. After a diffuse bounce,
                    // emitter NEE already accounted for it — skip to avoid double
                    // counting.
                    bool countEmissionOnHit = true;
                    bool alive = true;

                    for (uint32_t depth = 0; depth <= in.maxDepth && alive; ++depth)
                    {
                        // tmax 1e8 (was 1000, which clipped large USD scenes —
                        // the path tracer couldn't see past 1000 units). Far
                        // beyond any realistic scene; must match the Metal
                        // backend's r.max_distance for pt_backend_compare parity.
                        const auto hit = m_tlas->intersect(ray, 0.01f, 1.0e8f, RAY_FLAG_NONE);

                        if (!hit)
                        {
                            const glm::vec3 sky = skyRadiance(m_lights, in.lightCount, ray.direction);
                            // Primary miss: the env colour is the albedo guide for
                            // the background (helps the denoiser); other AOVs stay 0.
                            if (aovs && !capturedPrimary)
                            {
                                aovSample[static_cast<size_t>(AovKind::Albedo)] = glm::vec4(sky, 1.0f);
                                capturedPrimary = true;
                            }
                            accum += color * sky;
                            alive = false;
                            break;
                        }

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
                        const float NdotV_raw = glm::dot(N_raw, V);
                        const bool entering = NdotV_raw >= 0.0f;
                        // Robust shading normal (Schüssler 2017): when the
                        // interpolated normal bends past the silhouette
                        // (N_raw·V<0) the old code flipped it inward, killing sky
                        // GI / NEE → dark rim (amplified by clearcoat). Reflect it
                        // back to the view horizon instead — keeps N·V>0 (no
                        // grazing specular spike) and the surface lit. Front faces
                        // are untouched; glass flips the raw normal locally below.
                        const glm::vec3 N = (NdotV_raw < 0.0f)
                                                ? glm::normalize(N_raw - 2.0f * NdotV_raw * V)
                                                : N_raw;

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
                        // R3 clear coat: a clear dielectric (F0=0.04) GGX layer over the base.
                        const float clearcoat =
                            glm::clamp(gm.clearcoatFactor, 0.0f, 1.0f);
                        const float clearcoatRoughness = gm.clearcoatRoughnessFactor;
                        // R3 sheen: grazing retroreflective term added to the
                        // diffuse NEE. Purely additive (0 = off, no RNG draw).
                        const float sheen = std::max(gm.sheenFactor, 0.0f);
                        // R3d subsurface: wrap-diffusion weight + scatter tint
                        // (mirrors the MSL backend; 0 = off, diffuse unchanged).
                        const float subsurface = glm::clamp(gm.subsurfaceFactor, 0.0f, 1.0f);
                        const glm::vec3 subsurfaceColor(gm.subsurfaceColorR,
                                                        gm.subsurfaceColorG,
                                                        gm.subsurfaceColorB);
                        // R3 anisotropy: gated GGX stretch along the UV tangent
                        // (mirrors MSL; 0 = isotropic, existing path untouched).
                        const float anisotropy = glm::clamp(gm.anisotropyFactor, -1.0f, 1.0f);
                        // UV-aligned tangent frame for the anisotropic lobe
                        // (computed only when needed; mirrors the MSL backend).
                        glm::vec3 Taniso = T, Baniso = B;
                        if (anisotropy != 0.0f)
                        {
                            Taniso = computeUVTangent(
                                glm::vec3(m_positions[base + 0u]),
                                glm::vec3(m_positions[base + 1u]),
                                glm::vec3(m_positions[base + 2u]),
                                m_uvs[base + 0u], m_uvs[base + 1u], m_uvs[base + 2u], N, T);
                            Baniso = glm::cross(N, Taniso);
                        }

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

                        // First shaded surface = primary visibility for AOVs.
                        if (aovs && !capturedPrimary)
                        {
                            capturedPrimary = true;
                            aovSample[static_cast<size_t>(AovKind::Albedo)] = glm::vec4(albedo, 1.0f);
                            aovSample[static_cast<size_t>(AovKind::Normal)] = glm::vec4(N, 0.0f);
                            aovSample[static_cast<size_t>(AovKind::Depth)] =
                                glm::vec4(glm::length(hitPos - in.cameraPosition), 0.0f, 0.0f, 0.0f);
                            aovSample[static_cast<size_t>(AovKind::Position)] = glm::vec4(hitPos, 1.0f);
                            aovSample[static_cast<size_t>(AovKind::Emission)] = glm::vec4(emission, 1.0f);
                            aovSample[static_cast<size_t>(AovKind::InstanceId)] =
                                glm::vec4(static_cast<float>(instanceIdx + 1u), 0.0f, 0.0f, 0.0f);
                        }

                        // Emitted radiance — counted on direct arrival only when
                        // emitter NEE couldn't have sampled it (camera ray /
                        // post-specular), else the NEE below handles it.
                        if (countEmissionOnHit && glm::length(emission) > 1e-6f)
                        {
                            accum += color * emission;
                        }

                        if (in.lightCount > 0 && !isGlass)
                        {
                            const auto *slots = reinterpret_cast<const glm::vec4 *>(m_lights.data());
                            const glm::vec3 diffuseBrdf =
                                albedo * (1.0f - metallic) * (1.0f / 3.14159265f);
                            // Single-sample NEE: pick ONE analytic light uniformly
                            // and weight by lightCount, rather than looping every
                            // light (O(lightCount) shadow rays per hit — fatal on a
                            // 500+-light scene like Old Attic). Unbiased. Consumes
                            // exactly one nextRandom() here, matching the Metal
                            // backend so pt_backend_compare parity holds.
                            const uint32_t nl = in.lightCount;
                            const uint32_t li =
                                std::min(static_cast<uint32_t>(nextRandom(seed) * nl), nl - 1u);
                            const glm::vec4 posType = slots[li * 6u + 0u];
                            const glm::vec4 dirIntens = slots[li * 6u + 1u];
                            const glm::vec4 colorExtra = slots[li * 6u + 2u];

                            const int ltype = static_cast<int>(posType.w);
                            if (ltype != 2)  // skip Dome (handled by the miss shader)
                            {
                                glm::vec3 Ldir;
                                float falloff;
                                float lightDist;  // shadow-ray reach
                                if (ltype == 0)
                                {
                                    const glm::vec3 toLight = glm::vec3(posType) - hitPos;
                                    const float distSq = std::max(glm::dot(toLight, toLight), 1e-4f);
                                    const float rad = colorExtra.w;
                                    Ldir = toLight * (1.0f / std::sqrt(distSq));
                                    falloff = 1.0f / (distSq + rad * rad);
                                    lightDist = std::sqrt(distSq);
                                }
                                else if (ltype == 3)
                                {
                                    const float aw = colorExtra.w;
                                    const float ah = slots[li * 6u + 3u].w;
                                    Ldir = -glm::normalize(glm::vec3(dirIntens));
                                    falloff = std::max(aw * ah, 1e-4f);
                                    lightDist = glm::length(glm::vec3(posType) - hitPos);
                                }
                                else
                                {
                                    Ldir = -glm::normalize(glm::vec3(dirIntens));
                                    falloff = 1.0f;
                                    lightDist = 1.0e6f;  // distant/sun
                                }

                                // Subsurface wrap extends the lit band past the
                                // terminator by `subsurface`; at subsurface==0
                                // this is the exact old `dot(N,Ldir) <= 0`
                                // early-out (parity-safe).
                                const float rawNdotL = glm::dot(N, Ldir);
                                if (rawNdotL > -subsurface)
                                {
                                    const float NdotLlight = std::max(rawNdotL, 0.0f);

                                    // Shadow ray: skip if the light is occluded.
                                    Ray sray;
                                    sray.origin = hitPos + N * 0.001f;
                                    sray.direction = Ldir;
                                    sray.invDirection = glm::vec3(1.0f) / sray.direction;
                                    sray.time = sampleTime;
                                    if (!m_tlas->intersect(sray, 0.001f,
                                                           std::max(lightDist - 0.002f, 0.002f),
                                                           RAY_FLAG_NONE))
                                    {
                                        const glm::vec3 Li = glm::vec3(colorExtra) * dirIntens.w * falloff;
                                        const glm::vec3 Hs = glm::normalize(Ldir + V);
                                        const float sheenBrdf =
                                            sheen * std::pow(1.0f - std::max(glm::dot(Ldir, Hs), 0.0f), 5.0f);
                                        // Wrap-diffusion: blend Lambertian with a softened,
                                        // tinted response. mix(...,0) == Lambertian, so
                                        // subsurface==0 keeps the diffuse NEE bit-identical.
                                        const float wd = 1.0f + subsurface;
                                        const float wrapCos = glm::clamp(
                                            (rawNdotL + subsurface) / (wd * wd), 0.0f, 1.0f);
                                        const glm::vec3 sssResp = subsurfaceColor *
                                            (1.0f - metallic) * (1.0f / 3.14159265f) * wrapCos;
                                        const glm::vec3 diffuseLobe =
                                            glm::mix(diffuseBrdf * NdotLlight, sssResp, subsurface);
                                        // * nl: weight the single picked light by the count.
                                        accum += color * (diffuseLobe + sheenBrdf * NdotLlight) *
                                                 Li * static_cast<float>(nl);
                                    }
                                }
                            }
                        }

                        // Emissive area lights (NEE): sample one emissive
                        // triangle, shadow-test it, and add its contribution.
                        // Lets glowing geometry light the scene with far less
                        // noise than waiting for random bounces to hit it.
                        if (!m_emitters.empty() && !isGlass)
                        {
                            const glm::vec3 diffuseBrdf =
                                albedo * (1.0f - metallic) * (1.0f / 3.14159265f);
                            const uint32_t ne = static_cast<uint32_t>(m_emitters.size());
                            const uint32_t ei =
                                std::min(static_cast<uint32_t>(nextRandom(seed) * ne), ne - 1u);
                            const auto &E = m_emitters[ei];
                            // Uniform point on the triangle.
                            const float su = std::sqrt(nextRandom(seed));
                            const float b1 = 1.0f - su;
                            const float b2 = nextRandom(seed) * su;
                            const glm::vec3 y = E.p0 + b1 * (E.p1 - E.p0) + b2 * (E.p2 - E.p0);
                            const glm::vec3 toL = y - hitPos;
                            const float dist2 = std::max(glm::dot(toL, toL), 1e-6f);
                            const float dist = std::sqrt(dist2);
                            const glm::vec3 wi = toL / dist;
                            const float rawNdotL = glm::dot(N, wi);
                            glm::vec3 Ng = glm::cross(E.p1 - E.p0, E.p2 - E.p0);
                            const float ngLen = glm::length(Ng);
                            // subsurface wrap extends the band past the terminator;
                            // at subsurface==0 this is the exact old `rawNdotL > 0`.
                            if (rawNdotL > -subsurface && ngLen > 1e-12f)
                            {
                                Ng /= ngLen;
                                const float cosL = std::abs(glm::dot(Ng, -wi));
                                if (cosL > 1e-4f)
                                {
                                    Ray sray;
                                    sray.origin = hitPos + N * 0.001f;
                                    sray.direction = wi;
                                    sray.invDirection = glm::vec3(1.0f) / sray.direction;
                                    sray.time = sampleTime;
                                    if (!m_tlas->intersect(sray, 0.001f, dist - 0.002f, RAY_FLAG_NONE))
                                    {
                                        // pdf_A = 1/(area * ne); to solid angle:
                                        // ×dist²/cosL. Estimator divides f·Le·NdotL by it.
                                        const float w = E.area * static_cast<float>(ne) * cosL / dist2;
                                        const glm::vec3 Hs = glm::normalize(wi + V);
                                        const float sheenBrdf =
                                            sheen * std::pow(1.0f - std::max(glm::dot(wi, Hs), 0.0f), 5.0f);
                                        const float NdotL = std::max(rawNdotL, 0.0f);
                                        const float wd = 1.0f + subsurface;
                                        const float wrapCos = glm::clamp(
                                            (rawNdotL + subsurface) / (wd * wd), 0.0f, 1.0f);
                                        const glm::vec3 sssResp = subsurfaceColor *
                                            (1.0f - metallic) * (1.0f / 3.14159265f) * wrapCos;
                                        const glm::vec3 diffuseLobe =
                                            glm::mix(diffuseBrdf * NdotL, sssResp, subsurface);
                                        accum += color * (diffuseLobe + sheenBrdf * NdotL) * E.emission * w;
                                    }
                                }
                            }
                        }

                        const float r1 = nextRandom(seed);
                        const float r2 = nextRandom(seed);
                        const float r3 = nextRandom(seed);

                        glm::vec3 L;
                        glm::vec3 throughput;
                        const float NdotV = std::max(glm::dot(N, V), 0.001f);

                        // Clear coat decision (gated on clearcoat>0 so non-coat
                        // materials draw no extra RNG and render identically).
                        // Selection prob Fc = clearcoat·F_schlick(0.04); since
                        // this integrator uses the selection prob as the blend
                        // weight, the base is auto-attenuated by (1-Fc).
                        bool coatBounce = false;
                        if (clearcoat > 0.0f)
                        {
                            const float fc0 =
                                0.04f + 0.96f * std::pow(glm::clamp(1.0f - NdotV, 0.0f, 1.0f), 5.0f);
                            if (nextRandom(seed) < clearcoat * fc0) coatBounce = true;
                        }

                        if (coatBounce)
                        {
                            // White dielectric GGX coat at clearcoatRoughness.
                            const float ccR = glm::clamp(clearcoatRoughness, 0.04f, 1.0f);
                            const glm::vec3 H_local = sampleGGX(r1, r2, ccR);
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
                            const float G = geometrySmith(NdotV, NdotL, ccR);
                            throughput = glm::vec3(G * VdotH / (NdotV * NdotH));
                        }
                        else if (isGlass)
                        {
                            // Dielectric: glass tint is the surface baseColor on
                            // transmitted light (glTF KHR_transmission thin model).
                            // Glass is two-sided — orient the raw normal to the
                            // ray (old view-flipped normal; uses N_raw, not bent N).
                            const glm::vec3 gN = entering ? N_raw : -N_raw;
                            const float etaI = entering ? 1.0f : ior;
                            const float etaT = entering ? ior : 1.0f;
                            const float eta = etaI / etaT;
                            const float cosI = glm::clamp(glm::dot(gN, V), 0.0f, 1.0f);
                            const float F = fresnelDielectric(cosI, etaI, etaT);

                            if (r3 < F)
                            {
                                L = glm::reflect(incomingDir, gN);
                                throughput = albedo;
                            }
                            else
                            {
                                const glm::vec3 refracted = glm::refract(incomingDir, gN, eta);
                                if (glm::dot(refracted, refracted) < 1.0e-6f)
                                {
                                    L = glm::reflect(incomingDir, gN); // total internal reflection
                                    throughput = albedo;
                                }
                                else
                                {
                                    L = glm::normalize(refracted);
                                    const float etaScale = (etaT * etaT) / (etaI * etaI);
                                    throughput = albedo * transmission * etaScale;
                                }
                            }
                        }
                        else if (r3 < metallic)
                        {
                            // Anisotropy stretches the GGX highlight along the UV
                            // tangent. Gated: anisotropy==0 takes the exact
                            // isotropic path so existing metal renders are unchanged.
                            glm::vec3 Hv;
                            if (anisotropy != 0.0f)
                            {
                                const float alpha = roughness * roughness;
                                const float aspect =
                                    std::sqrt(std::max(1.0f - 0.9f * std::abs(anisotropy), 1e-4f));
                                float aT = std::max(alpha / aspect, 1e-4f);
                                float aB = std::max(alpha * aspect, 1e-4f);
                                if (anisotropy < 0.0f) std::swap(aT, aB);
                                const glm::vec3 H_local = sampleGGXAniso(r1, r2, aT, aB);
                                Hv = glm::normalize(tangentToWorld(H_local, N, Taniso, Baniso));
                            }
                            else
                            {
                                const glm::vec3 H_local = sampleGGX(r1, r2, roughness);
                                Hv = glm::normalize(tangentToWorld(H_local, N, T, B));
                            }
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

                        // Gate next-hit emission: a diffuse bounce's emitter
                        // contribution is already covered by NEE above, so don't
                        // double-count it on arrival. Specular/glossy (glass,
                        // metal) bounces aren't sampled by the diffuse NEE, so
                        // their emitter arrivals must still count.
                        countEmissionOnHit = coatBounce || isGlass || (r3 < metallic);

                        throughput = glm::clamp(throughput, glm::vec3(0.0f), glm::vec3(10.0f));
                        color *= throughput;

                        const glm::vec3 offsetN = (glm::dot(L, N) < 0.0f) ? -N : N;
                        ray.origin = hitPos + offsetN * 0.001f;
                        ray.direction = L;
                        ray.invDirection = glm::vec3(1.0f) / ray.direction;

                        // Russian roulette: past the first couple of bounces,
                        // terminate low-throughput paths probabilistically and
                        // scale the survivors by 1/p (unbiased — the expected
                        // contribution is unchanged), so deep near-black bounces
                        // stop costing rays. The start depth, the survival prob,
                        // and the single nextRandom draw MUST match the Metal
                        // backend (pathtrace_msl.hpp) exactly to keep the two in
                        // parity. Depths 0-1 are never rouletted (they carry most
                        // of the energy); the [0.05,0.95] clamp bounds both the
                        // firefly boost and the path length.
                        if (depth >= 2u)
                        {
                            const float p = glm::clamp(
                                std::max(color.x, std::max(color.y, color.z)), 0.05f, 0.95f);
                            if (nextRandom(seed) >= p) break;
                            color /= p;
                        }
                    }

                    // ── resolve ──
                    // All radiance (emission, sky, NEE) lives in `accum`;
                    // `color` is pure throughput, consumed during the walk.
                    const glm::vec3 sampleColor = accum;
                    const int n = (in.currentSample - 1) * static_cast<int>(samplesPerFrame) +
                                  static_cast<int>(s) + 1;
                    mean = mean + (sampleColor - mean) / static_cast<float>(n);

                    if (aovs)
                        for (size_t k = 0; k < kAovN; ++k)
                            aovMean[k] += (aovSample[k] - aovMean[k]) / static_cast<float>(n);
                }

                m_accumulator[pixelIdx] = glm::vec4(mean, 1.0f);
                if (aovs)
                    for (size_t k = 0; k < kAovN; ++k) m_aovs[k][pixelIdx] = aovMean[k];

                const glm::vec3 tonemapped = mean / (mean + glm::vec3(1.0f));
                const glm::vec3 gammaCorrected =
                    glm::pow(tonemapped, glm::vec3(1.0f / 2.2f));
                // Linear output (EXR/denoise) skips tonemap+gamma and writes
                // raw radiance; display output stays tonemapped + gamma.
                const glm::vec3 outRGB = m_config->linearOutput
                                             ? glm::max(mean, glm::vec3(0.0f))
                                             : gammaCorrected;

                if (m_config->hdrOutput)
                {
                    auto *out = reinterpret_cast<float *>(m_pixels.data()) + pixelIdx * 4;
                    out[0] = outRGB.r;
                    out[1] = outRGB.g;
                    out[2] = outRGB.b;
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

    // One-shot denoise of the converged accumulation (see PathTracerBackend::
    // denoise — invoked once max samples is reached, not per frame). m_accumulator
    // IS the linear beauty (one RGBA32F per pixel), so OIDN reads it with no
    // readback; guided by albedo + normal when AOVs are on. The denoised linear
    // is tonemapped over the display buffer (m_pixels) with the same Reinhard +
    // gamma 2.2 as the per-sample resolve. Skipped under linearOutput so it never
    // touches the EXR / host-side export-denoise path.
    bool CpuPathTracerBackend::denoise()
    {
        if (!m_config || m_config->linearOutput || !tracey::denoiserAvailable())
            return false;
        const size_t pixelCount =
            static_cast<size_t>(m_config->width) * m_config->height;
        if (pixelCount == 0 || m_accumulator.size() != pixelCount) return false;

        m_denoiseScratch.resize(pixelCount);
        const float *albedo = nullptr;
        const float *normal = nullptr;
        if (m_config->enableAovs)
        {
            const auto &aAov = m_aovs[static_cast<size_t>(AovKind::Albedo)];
            const auto &nAov = m_aovs[static_cast<size_t>(AovKind::Normal)];
            if (aAov.size() == pixelCount)
                albedo = reinterpret_cast<const float *>(aAov.data());
            if (nAov.size() == pixelCount)
                normal = reinterpret_cast<const float *>(nAov.data());
        }
        if (!tracey::denoiseImage(static_cast<int>(m_config->width),
                                  static_cast<int>(m_config->height),
                                  reinterpret_cast<const float *>(m_accumulator.data()),
                                  albedo, normal,
                                  reinterpret_cast<float *>(m_denoiseScratch.data())))
            return false;

        const bool hdr = m_config->hdrOutput;
        parallel_for_chunks(pixelCount, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
            {
                const glm::vec3 c(m_denoiseScratch[i]);
                const glm::vec3 t = c / (c + glm::vec3(1.0f));
                const glm::vec3 g =
                    glm::pow(glm::max(t, glm::vec3(0.0f)), glm::vec3(1.0f / 2.2f));
                if (hdr)
                {
                    auto *out = reinterpret_cast<float *>(m_pixels.data()) + i * 4;
                    out[0] = g.r; out[1] = g.g; out[2] = g.b; out[3] = 1.0f;
                }
                else
                {
                    auto *out = m_pixels.data() + i * 4;
                    out[0] = static_cast<uint8_t>(glm::clamp(g.r, 0.0f, 1.0f) * 255.0f + 0.5f);
                    out[1] = static_cast<uint8_t>(glm::clamp(g.g, 0.0f, 1.0f) * 255.0f + 0.5f);
                    out[2] = static_cast<uint8_t>(glm::clamp(g.b, 0.0f, 1.0f) * 255.0f + 0.5f);
                    out[3] = 255;
                }
            }
        });
        return true;
    }

    size_t CpuPathTracerBackend::readback(void *dst)
    {
        std::memcpy(dst, m_pixels.data(), m_pixels.size());
        return m_pixels.size();
    }

    bool CpuPathTracerBackend::aovsAvailable() const
    {
        return m_config && m_config->enableAovs;
    }

    size_t CpuPathTracerBackend::readbackAOV(AovKind aov, void *dst)
    {
        const size_t idx = static_cast<size_t>(aov);
        if (!m_config || !m_config->enableAovs || idx >= m_aovs.size()) return 0;
        const auto &buf = m_aovs[idx];
        if (buf.empty()) return 0;
        const size_t bytes = buf.size() * sizeof(glm::vec4);
        std::memcpy(dst, buf.data(), bytes);
        return bytes;
    }
} // namespace tracey
