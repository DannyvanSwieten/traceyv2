#include "path_tracer.hpp"
#include "../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"

#include <chrono>
#include <stdexcept>
#include <span>

namespace tracey
{
    PathTracer::PathTracer(Device *device, const PathTracerConfig &config)
        : m_device(device), m_config(config)
    {
        if (!m_device)
        {
            throw std::runtime_error("PathTracer: device cannot be null");
        }

        createOutputImage();
        buildPipeline();
        allocateDescriptorSets();
    }

    PathTracer::~PathTracer() = default;

    void PathTracer::createOutputImage()
    {
        ImageFormat format = m_config.hdrOutput
                                 ? ImageFormat::R32G32B32A32Sfloat
                                 : ImageFormat::R8G8B8A8Unorm;

        m_outputImage.reset(m_device->createImage2D(m_config.width, m_config.height, format));

        // Linear HDR accumulator: lives across frames so the running mean is
        // numerically stable (the resolve shader reads/writes this, and only
        // tonemaps into outputImage for display).
        m_accumulatorImage.reset(m_device->createImage2D(m_config.width, m_config.height,
                                                        ImageFormat::R32G32B32A32Sfloat));

        // Create readback buffer
        size_t pixelSize = m_config.hdrOutput ? 16 : 4; // 4 floats or 4 bytes
        size_t bufferSize = m_config.width * m_config.height * pixelSize;
        m_readbackBuffer.reset(m_device->createBuffer(bufferSize, BufferUsage::TransferDst));
    }

    void PathTracer::buildPipeline()
    {
        m_pipelineBuilder = std::make_unique<ISFPipelineBuilder>(*m_device);

        // Load ISF shaders
        m_pipelineBuilder->addRayGenShader(m_config.rayGenShader);
        m_pipelineBuilder->addHitShader(m_config.hitShader);
        m_pipelineBuilder->addMissShader(m_config.missShader);
        m_pipelineBuilder->addResolveShader(m_config.resolveShader);

        // Create shader inputs buffer based on ISF inputs
        m_shaderInputs = std::make_unique<ShaderInputsBuffer>(
            m_device, m_pipelineBuilder->getInputsLayout());

        // Build pipeline layout with API-specified resources
        // ISF builder will add payload buffers from shader definitions
        // Layout must persist as a member variable so the pipeline can reference it
        m_pipelineLayout = std::make_unique<RayTracingPipelineLayoutDescriptor>();
        m_pipelineLayout->addImage2D("outputImage", ShaderStage::RayGeneration,
                                     m_config.hdrOutput ? ImageLayoutFormat::RGBA32F
                                                        : ImageLayoutFormat::RGBA8);
        // Linear HDR accumulator, written + read by the resolve shader for
        // numerically stable cross-frame averaging.
        m_pipelineLayout->addImage2D("accumulatorImage", ShaderStage::Resolve,
                                     ImageLayoutFormat::RGBA32F);
        m_pipelineLayout->addAccelerationStructure("tlas", ShaderStage::RayGeneration);

        // Add API-specified buffers for scene data
        StructureLayout vertexStructure("Vertex");
        vertexStructure.addMember({"positions", "vec3", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("vertexBuffer", ShaderStage::ClosestHit, vertexStructure);

        StructureLayout materialStructure("MaterialData");
        materialStructure.addMember({"data", "int", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("materials", ShaderStage::ClosestHit, materialStructure);

        StructureLayout uvStructure("UVData");
        uvStructure.addMember({"uvs", "vec2", 0, true, 0});
        m_pipelineLayout->addStorageBuffer("uvBuffer", ShaderStage::ClosestHit, uvStructure);

        // Bindless texture support: separate samplers and image array
        // Using 256 textures (maxPerStageDescriptorSampledImages limit on macOS)
        m_pipelineLayout->addSampler("linearSampler", ShaderStage::ClosestHit);
        m_pipelineLayout->addSampler("nearestSampler", ShaderStage::ClosestHit);
        m_pipelineLayout->addSampledImageArray("textures", ShaderStage::ClosestHit, 256);

        m_pipeline = m_pipelineBuilder->build(*m_pipelineLayout);
    }

    void PathTracer::allocateDescriptorSets()
    {
        std::array<DescriptorSet *, 2> descriptorSetPtrs;
        m_pipeline->allocateDescriptorSets(descriptorSetPtrs);

        for (size_t i = 0; i < 2; ++i)
        {
            m_descriptorSets[i].reset(descriptorSetPtrs[i]);
        }
    }

    void PathTracer::bindSceneResources(const SceneCompiler::CompiledScene &scene)
    {
        // Bind resources that may change between renders
        for (auto &descriptorSet : m_descriptorSets)
        {
            descriptorSet->setImage2D("outputImage", m_outputImage.get());
            descriptorSet->setImage2D("accumulatorImage", m_accumulatorImage.get());
            descriptorSet->setAccelerationStructure("tlas", scene.tlas.get());

            if (!scene.vertexBuffers.empty())
            {
                descriptorSet->setBuffer("vertexBuffer", scene.vertexBuffers[0].get());
            }

            if (scene.materialBuffer)
            {
                descriptorSet->setBuffer("materials", scene.materialBuffer.get());
            }

            if (scene.uvBuffer)
            {
                descriptorSet->setBuffer("uvBuffer", scene.uvBuffer.get());
            }

            // Bind samplers for bindless texture support
            descriptorSet->setSampler("linearSampler", true);   // Linear filtering
            descriptorSet->setSampler("nearestSampler", false); // Nearest filtering

            if (!scene.textures.empty())
            {
                std::vector<Image2D *> texturePtrs;
                for (const auto &tex : scene.textures)
                {
                    texturePtrs.push_back(tex.get());
                }
                descriptorSet->setSampledImageArray("textures", std::span<Image2D *>(texturePtrs));
            }

            descriptorSet->setUniformBuffer("shaderInputs", m_shaderInputs->buffer());
        }
    }

    void PathTracer::updateCameraUniforms(const Camera &camera)
    {
        m_shaderInputs->setFloat("fov", camera.fov());
        m_shaderInputs->setVec3("cameraPosition", camera.position());
        m_shaderInputs->setVec3("cameraForward", camera.forward());
        m_shaderInputs->setVec3("cameraRight", camera.right());
        m_shaderInputs->setVec3("cameraUp", camera.up());
        m_shaderInputs->setInt("currentSample", m_sampleCount + 1);
        m_shaderInputs->setUint("maxDepth", m_config.maxBounces);
        m_shaderInputs->upload();
    }

    double PathTracer::render(const SceneCompiler::CompiledScene &scene,
                              const Camera &camera,
                              bool clearAccumulation)
    {
        if (clearAccumulation)
        {
            m_sampleCount = 0;
        }

        // Bind scene resources
        bindSceneResources(scene);

        // Update camera parameters
        updateCameraUniforms(camera);

        // Create/reset command buffer
        if (!m_commandBuffer)
        {
            m_commandBuffer.reset(m_device->createRayTracingCommandBuffer());
        }

        m_commandBuffer->begin();

        if (clearAccumulation)
        {
            // Clear the accumulator, not the display image. The resolve shader
            // reads/writes the accumulator across samples; outputImage is just
            // the latest tonemapped snapshot and is always overwritten by
            // resolve, so clearing it is unnecessary.
            m_commandBuffer->clearImage(m_accumulatorImage.get(), 0.0f, 0.0f, 0.0f, 0.0f);
        }

        m_commandBuffer->setPipeline(m_pipeline.get());
        m_commandBuffer->setDescriptorSet(m_descriptorSets[0].get());
        m_commandBuffer->setDescriptorSet(m_descriptorSets[1].get());

        TraceRaysParams traceParams;
        traceParams.samplesPerFrame = m_config.samplesPerFrame;
        traceParams.maxBounces = m_config.maxBounces;
        m_commandBuffer->traceRays(*m_pipelineBuilder->getShaderBindingTable(),
                                   m_config.width, m_config.height, traceParams);
        m_commandBuffer->copyImageToBuffer(m_outputImage.get(), m_readbackBuffer.get());
        m_commandBuffer->end();

        auto startTime = std::chrono::high_resolution_clock::now();
        m_commandBuffer->waitUntilCompleted();
        auto endTime = std::chrono::high_resolution_clock::now();

        m_sampleCount++;

        std::chrono::duration<double, std::milli> executionTime = endTime - startTime;
        return executionTime.count();
    }

    size_t PathTracer::readback(void *outData)
    {
        size_t pixelSize = m_config.hdrOutput ? 16 : 4;
        size_t bufferSize = m_config.width * m_config.height * pixelSize;

        const void *gpuData = m_readbackBuffer->mapForReading();
        std::memcpy(outData, gpuData, bufferSize);
        m_readbackBuffer->unmap();

        return bufferSize;
    }
} // namespace tracey
