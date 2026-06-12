#include "wavefront_compute_backend.hpp"

#include "path_tracer/api/path_tracer.hpp"
#include "ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "gpu/vulkan_queue_sync.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>

namespace tracey
{
    WavefrontComputeBackend::WavefrontComputeBackend() = default;
    WavefrontComputeBackend::~WavefrontComputeBackend() = default;

    void WavefrontComputeBackend::initialize(const InitParams &params)
    {
        if (!params.device || !params.config || !params.outputImage ||
            !params.accumulatorImage || !params.readbackBuffer ||
            !params.shaderInputs || !params.shaderInputsLayout)
        {
            throw std::runtime_error("WavefrontComputeBackend::initialize missing required InitParams");
        }

        m_device = params.device;
        m_config = params.config;
        m_outputImage = params.outputImage;
        m_accumulatorImage = params.accumulatorImage;
        m_readbackBuffer = params.readbackBuffer;
        m_shaderInputs = params.shaderInputs;
        m_shaderInputsLayout = params.shaderInputsLayout;

        buildPipeline();
        allocateDescriptorSets();

        if (m_config->useMaterialPrograms)
        {
            // Seed with a single passthrough program at index 0. Callers can
            // override later via setMaterialPrograms.
            MaterialProgramBuffer pb;
            pb.addProgram(makePassthroughProgram());
            uploadMaterialPrograms(pb);
        }
    }

    void WavefrontComputeBackend::buildPipeline()
    {
        m_pipelineBuilder = std::make_unique<RayTracingShaderBuilder>(*m_device);

        m_pipelineBuilder->setRayGenShader(m_config->rayGenShader);
        m_pipelineBuilder->setHitShader(m_config->hitShader);
        m_pipelineBuilder->setMissShader(m_config->missShader);
        m_pipelineBuilder->setResolveShader(m_config->resolveShader);

        m_pipelineLayout = std::make_unique<RayTracingPipelineLayoutDescriptor>();

        // Wavefront per-ray state. The wavefront pipeline compiler emits a
        // buffer at binding 60 keyed off this declaration.
        StructureLayout payloadLayout("RayPayload");
        payloadLayout.addMember({"color",       "vec3", 0, false, 0});
        payloadLayout.addMember({"direction",   "vec3", 0, false, 0});
        // Direct-lighting (NEE) accumulator. Hit shader adds shadowless
        // light contributions here; resolve sums color + accum so the
        // existing throughput pipeline keeps working unchanged.
        payloadLayout.addMember({"accum",       "vec3", 0, false, 0});
        payloadLayout.addMember({"depth",       "uint", 0, false, 0});
        payloadLayout.addMember({"alive",       "bool", 0, false, 0});
        payloadLayout.addMember({"sampleIndex", "uint", 0, false, 0});
        payloadLayout.addMember({"rngSeed",     "uint", 0, false, 0});
        m_pipelineLayout->addPayload("rayPayload", ShaderStage::RayGeneration, payloadLayout);

        // Global render state. The layout is owned by the façade so the
        // ShaderInputsBuffer construction agrees byte-for-byte with what the
        // shader expects.
        m_pipelineLayout->addUniformBuffer("shaderInputs", ShaderStage::RayGeneration,
                                           *m_shaderInputsLayout);

        m_pipelineLayout->addImage2D("outputImage", ShaderStage::RayGeneration,
                                     m_config->hdrOutput ? ImageLayoutFormat::RGBA32F
                                                         : ImageLayoutFormat::RGBA8);
        m_pipelineLayout->addImage2D("accumulatorImage", ShaderStage::Resolve,
                                     ImageLayoutFormat::RGBA32F);
        m_pipelineLayout->addAccelerationStructure("tlas", ShaderStage::RayGeneration);

        StructureLayout vertexStructure("Vertex");
        vertexStructure.addMember({"positions", "vec3", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("vertexBuffer", ShaderStage::ClosestHit, vertexStructure);

        StructureLayout materialStructure("MaterialData");
        materialStructure.addMember({"data", "int", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("materials", ShaderStage::ClosestHit, materialStructure);

        StructureLayout uvStructure("UVData");
        uvStructure.addMember({"uvs", "vec2", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("uvBuffer", ShaderStage::ClosestHit, uvStructure);

        // Per-vertex normals — same per-vertex indexing as the UV buffer,
        // looked up at hit time via instanceData.data[i].y + triIdx*3 + i. The
        // hit shader interpolates these to get a smooth surface normal
        // (or a per-face one, when the upstream Normal SOP wrote them in
        // flat mode). Falls back to the BLAS face normal when the buffer
        // wasn't created (no object in the scene carries N).
        //
        // Declared as vec4 (not vec3) to dodge the std430 array stride
        // trap — `vec3 arr[]` has a 16-byte stride per element, which
        // wouldn't match a packed std::vector<Vec3>. We store xyz and
        // ignore w on the upload side; the shader reads `.xyz`.
        StructureLayout normalStructure("NormalData");
        normalStructure.addMember({"normals", "vec4", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("normalBuffer", ShaderStage::ClosestHit, normalStructure);

        // Per-instance (programId, uvOffset) packed into a single uvec2[]
        // SSBO. instanceData.data[hit.instanceIndex].x → programId for the
        // material VM and the wavefront sort bin key; .y → base offset
        // into uvBuffer/normalBuffer so a BLAS-local triangleIndex resolves
        // to the right slice (without it, multi-object scenes alias every
        // instance to BLAS 0's vertices). Packed because the wavefront
        // pipeline already runs against MoltenVK's per-stage storage buffer
        // descriptor cap of 31; two separate uint[] buffers would push us
        // one over.
        StructureLayout instanceDataStructure("InstanceData");
        instanceDataStructure.addMember({"data", "uvec2", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("instanceData", ShaderStage::ClosestHit, instanceDataStructure);

        // Scene lights. The hit shader iterates `shaderInputs.lightCount`
        // entries and samples each (point: 1/r² falloff, distant: parallel
        // ray). Single unbounded vec4 array indexed in groups of 3 — see
        // GPULight in scene_compiler.hpp for the slot layout.
        StructureLayout lightStructure("LightData");
        lightStructure.addMember({"data", "vec4", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("lights", ShaderStage::ClosestHit, lightStructure);

        // Four sampler combos: filter (linear/nearest) × wrap (repeat/clamp).
        // The hit shader picks one per texture access from a 2-bit field on
        // the material; glTF's per-texture sampler info round-trips through
        // here unchanged.
        m_pipelineLayout->addSampler("linearRepeatSampler",  ShaderStage::ClosestHit);
        m_pipelineLayout->addSampler("linearClampSampler",   ShaderStage::ClosestHit);
        m_pipelineLayout->addSampler("nearestRepeatSampler", ShaderStage::ClosestHit);
        m_pipelineLayout->addSampler("nearestClampSampler",  ShaderStage::ClosestHit);
        // Cap by the device's bindless texture budget. On macOS/MoltenVK the
        // whole compute pipeline's resources must fit under maxPerStageResources
        // (287), which leaves ~220 slots once the wavefront's fixed bindings
        // are accounted for; on a desktop GPU this resolves to thousands.
        const uint32_t kRequestedTextures = 256;
        const uint32_t textureCount = std::min(kRequestedTextures, m_device->maxBindlessTextures());
        m_pipelineLayout->addSampledImageArray("textures", ShaderStage::ClosestHit, textureCount);

        if (m_config->useMaterialPrograms)
        {
            StructureLayout codeStruct("MaterialProgramCode");
            codeStruct.addMember({"code", "uvec4", 0, true, 0});
            m_pipelineLayout->addStorageBuffer("materialProgramCode", ShaderStage::ClosestHit, codeStruct);

            StructureLayout constStruct("MaterialProgramConstants");
            constStruct.addMember({"constants", "vec4", 0, true, 0});
            m_pipelineLayout->addStorageBuffer("materialProgramConstants", ShaderStage::ClosestHit, constStruct);

            StructureLayout headerStruct("MaterialProgramHeaders");
            headerStruct.addMember({"headers", "uvec4", 0, true, 0});
            m_pipelineLayout->addStorageBuffer("materialProgramHeaders", ShaderStage::ClosestHit, headerStruct);

            StructureLayout paramsStruct("MaterialParameters");
            paramsStruct.addMember({"parameters", "vec4", 0, true, 0});
            m_pipelineLayout->addStorageBuffer("materialParameters", ShaderStage::ClosestHit, paramsStruct);
        }

        m_pipeline = m_pipelineBuilder->build(*m_pipelineLayout);
    }

    void WavefrontComputeBackend::allocateDescriptorSets()
    {
        std::array<DescriptorSet *, 2> descriptorSetPtrs;
        m_pipeline->allocateDescriptorSets(descriptorSetPtrs);

        for (size_t i = 0; i < 2; ++i)
        {
            m_descriptorSets[i].reset(descriptorSetPtrs[i]);
        }
    }

    void WavefrontComputeBackend::bindSceneResources(const SceneCompiler::CompiledScene &scene)
    {
        for (auto &descriptorSet : m_descriptorSets)
        {
            descriptorSet->setImage2D("outputImage", m_outputImage);
            descriptorSet->setImage2D("accumulatorImage", m_accumulatorImage);
            descriptorSet->setAccelerationStructure("tlas", scene.tlas.get());

            if (!scene.vertexBuffers.empty())
            {
                // BlasCache owns the buffer; CompiledScene stores observers,
                // so the const_cast strips the constness of the pointer (the
                // underlying buffer is still mutable on the cache side).
                descriptorSet->setBuffer("vertexBuffer",
                    const_cast<Buffer*>(scene.vertexBuffers[0]));
            }

            if (scene.materialBuffer)
            {
                descriptorSet->setBuffer("materials", scene.materialBuffer.get());
            }

            if (scene.uvBuffer)
            {
                descriptorSet->setBuffer("uvBuffer", scene.uvBuffer.get());
            }

            if (scene.normalBuffer)
            {
                descriptorSet->setBuffer("normalBuffer", scene.normalBuffer.get());
            }

            if (scene.instanceDataBuffer)
            {
                descriptorSet->setBuffer("instanceData", scene.instanceDataBuffer.get());
            }

            if (scene.lightBuffer)
            {
                descriptorSet->setBuffer("lights", scene.lightBuffer.get());
            }

            descriptorSet->setSampler("linearRepeatSampler",  SamplerKind::LinearRepeat);
            descriptorSet->setSampler("linearClampSampler",   SamplerKind::LinearClamp);
            descriptorSet->setSampler("nearestRepeatSampler", SamplerKind::NearestRepeat);
            descriptorSet->setSampler("nearestClampSampler",  SamplerKind::NearestClamp);

            if (!scene.textures.empty())
            {
                std::vector<Image2D *> texturePtrs;
                for (const auto &tex : scene.textures)
                {
                    texturePtrs.push_back(tex.get());
                }
                descriptorSet->setSampledImageArray("textures", std::span<Image2D *>(texturePtrs));
            }

            if (m_config->useMaterialPrograms && m_programCodeBuffer)
            {
                descriptorSet->setBuffer("materialProgramCode", m_programCodeBuffer.get());
                descriptorSet->setBuffer("materialProgramConstants", m_programConstantsBuffer.get());
                descriptorSet->setBuffer("materialProgramHeaders", m_programHeadersBuffer.get());
                descriptorSet->setBuffer("materialParameters", m_programParametersBuffer.get());
            }

            descriptorSet->setUniformBuffer("shaderInputs", m_shaderInputs->buffer());
        }
    }

    double WavefrontComputeBackend::dispatch(const SceneCompiler::CompiledScene &scene,
                                             uint32_t /*accumulatedSampleCount*/,
                                             bool clearAccumulation,
                                             bool wantReadback)
    {
        // Whole-dispatch lock: covers command-buffer begin/record/end
        // and the submit + wait inside m_commandBuffer->end(). Same
        // rationale as Rasterizer::render — without it the cook
        // worker's compute dispatchers race the path-tracer recording
        // on the shared command pool and Vulkan validation rejects
        // it as a threading error.
        std::lock_guard<std::mutex> gpuLock(vulkanQueueMutex());

        bindSceneResources(scene);

        if (!m_commandBuffer)
        {
            m_commandBuffer.reset(m_device->createRayTracingCommandBuffer());
        }

        m_commandBuffer->begin();

        if (clearAccumulation)
        {
            // Clear the accumulator only — outputImage is fully overwritten by
            // the resolve shader every dispatch, so clearing it is wasted work.
            m_commandBuffer->clearImage(m_accumulatorImage, 0.0f, 0.0f, 0.0f, 0.0f);
        }

        m_commandBuffer->setPipeline(m_pipeline.get());
        m_commandBuffer->setDescriptorSet(m_descriptorSets[0].get());
        m_commandBuffer->setDescriptorSet(m_descriptorSets[1].get());

        TraceRaysParams traceParams;
        traceParams.samplesPerFrame = m_config->samplesPerFrame;
        traceParams.maxBounces = m_config->maxBounces;
        m_commandBuffer->traceRays(*m_pipelineBuilder->getShaderBindingTable(),
                                   m_config->width, m_config->height, traceParams);
        // The readback buffer is only consumed by the export / explicit
        // render_frame command paths. The live viewport composites
        // outputImage() straight into the swapchain and never maps the
        // readback buffer, so the full-frame copyImageToBuffer is pure
        // GPU time + a fence wait downstream for no consumer.
        if (wantReadback)
        {
            m_commandBuffer->copyImageToBuffer(m_outputImage, m_readbackBuffer);
        }
        m_commandBuffer->end();

        auto startTime = std::chrono::high_resolution_clock::now();
        m_commandBuffer->waitUntilCompleted();
        auto endTime = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::milli> executionTime = endTime - startTime;
        return executionTime.count();
    }

    void WavefrontComputeBackend::uploadMaterialPrograms(const MaterialProgramBuffer &programs)
    {
        if (!m_config->useMaterialPrograms)
        {
            throw std::runtime_error("WavefrontComputeBackend: uploadMaterialPrograms requires config.useMaterialPrograms");
        }
        if (programs.headers().empty())
        {
            throw std::runtime_error("WavefrontComputeBackend: program buffer must contain at least one program");
        }

        // SSBOs declared as runtime arrays must be non-empty. Pad each
        // potentially-empty buffer to one element so the binding stays valid.
        const size_t codeBytes = programs.codeBytes();
        const size_t constBytes = programs.constants().empty() ? sizeof(Vec4) : programs.constantsBytes();
        const size_t headerBytes = programs.headersBytes();
        const size_t paramBytes = programs.parameters().empty() ? sizeof(Vec4) : programs.parametersBytes();

        m_programCodeBuffer.reset(m_device->createBuffer(
            static_cast<uint32_t>(codeBytes), BufferUsage::StorageBuffer));
        m_programConstantsBuffer.reset(m_device->createBuffer(
            static_cast<uint32_t>(constBytes), BufferUsage::StorageBuffer));
        m_programHeadersBuffer.reset(m_device->createBuffer(
            static_cast<uint32_t>(headerBytes), BufferUsage::StorageBuffer));
        m_programParametersBuffer.reset(m_device->createBuffer(
            static_cast<uint32_t>(paramBytes), BufferUsage::StorageBuffer));

        std::memcpy(m_programCodeBuffer->mapForWriting(),
                    programs.code().data(), codeBytes);
        m_programCodeBuffer->flush();

        if (programs.constants().empty())
        {
            Vec4 zero(0.0f);
            std::memcpy(m_programConstantsBuffer->mapForWriting(), &zero, sizeof(Vec4));
        }
        else
        {
            std::memcpy(m_programConstantsBuffer->mapForWriting(),
                        programs.constants().data(), programs.constantsBytes());
        }
        m_programConstantsBuffer->flush();

        std::memcpy(m_programHeadersBuffer->mapForWriting(),
                    programs.headers().data(), headerBytes);
        m_programHeadersBuffer->flush();

        uploadMaterialParameters(programs);
    }

    void WavefrontComputeBackend::uploadMaterialParameters(const MaterialProgramBuffer &programs)
    {
        if (!m_programParametersBuffer) return;

        const auto &params = programs.parameters();
        if (params.empty())
        {
            Vec4 zero(0.0f);
            std::memcpy(m_programParametersBuffer->mapForWriting(), &zero, sizeof(Vec4));
        }
        else
        {
            std::memcpy(m_programParametersBuffer->mapForWriting(),
                        params.data(), params.size() * sizeof(Vec4));
        }
        m_programParametersBuffer->flush();
    }

    size_t WavefrontComputeBackend::readback(void *dst)
    {
        // Copy out of the façade-owned readback buffer the dispatch filled
        // (wantReadback=true enqueued the image→buffer copy; dispatch waits
        // for GPU completion before returning, and the buffer is
        // HOST_COHERENT, so mapForReading observes the finished frame).
        const size_t pixelSize = m_config->hdrOutput ? 16 : 4;
        const size_t bufferSize =
            static_cast<size_t>(m_config->width) * m_config->height * pixelSize;

        const void *gpuData = m_readbackBuffer->mapForReading();
        std::memcpy(dst, gpuData, bufferSize);
        m_readbackBuffer->unmap();

        return bufferSize;
    }
} // namespace tracey
