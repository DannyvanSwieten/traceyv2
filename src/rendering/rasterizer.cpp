#include "rasterizer.hpp"
#include "../device/buffer.hpp"
#include <chrono>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace tracey
{
    Rasterizer::Rasterizer(Device *device, const RasterizerConfig &config)
        : m_device(device), m_config(config)
    {
        if (!device)
        {
            throw std::runtime_error("Rasterizer: device cannot be null");
        }

        createPipeline();
    }

    Rasterizer::~Rasterizer() = default;

    void Rasterizer::createPipeline()
    {
        // Create empty pipeline layout (descriptors can be added later for textures/materials)
        m_pipelineLayout = std::make_unique<GraphicsPipelineLayout>();

        // Configure graphics pipeline
        GraphicsPipelineConfig pipelineConfig;
        pipelineConfig.width = m_config.width;
        pipelineConfig.height = m_config.height;
        pipelineConfig.vertexShader = m_config.vertexShader;
        pipelineConfig.fragmentShader = m_config.fragmentShader;
        pipelineConfig.colorFormat = m_config.colorFormat;
        pipelineConfig.useDepthBuffer = m_config.useDepthBuffer;
        pipelineConfig.depthTestEnable = m_config.depthTestEnable;
        pipelineConfig.cullBackFaces = m_config.cullBackFaces;
        pipelineConfig.alphaBlending = m_config.alphaBlending;

        // Create graphics pipeline
        m_pipeline.reset(m_device->createGraphicsPipeline(pipelineConfig, *m_pipelineLayout));

        // Get the output image from the pipeline's color target
        m_outputImage.reset(m_pipeline->colorTarget());

        // Create command buffer
        m_commandBuffer.reset(m_device->createGraphicsCommandBuffer());

        // Create readback buffer (for CPU access to rendered pixels)
        const size_t pixelSize = 4; // R8G8B8A8 = 4 bytes per pixel
        const size_t bufferSize = m_config.width * m_config.height * pixelSize;
        m_readbackBuffer.reset(m_device->createBuffer(
            static_cast<uint32_t>(bufferSize),
            BufferUsage::TransferDst));
    }

    double Rasterizer::render(const SceneCompiler::CompiledScene &scene,
                              const Camera &camera)
    {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Begin command recording
        m_commandBuffer->begin();

        // Begin render pass with clear color
        const float clearColor[4] = {0.2f, 0.3f, 0.4f, 1.0f};
        m_commandBuffer->beginRenderPass(
            m_pipeline.get(),
            clearColor[0], clearColor[1], clearColor[2], clearColor[3],
            1.0f  // clear depth
        );

        // Bind pipeline
        m_commandBuffer->bindPipeline(m_pipeline.get());

        // Update camera uniforms and render the scene
        updateCameraUniforms(camera);
        renderScene(scene);

        // End render pass and command recording
        m_commandBuffer->endRenderPass();
        m_commandBuffer->end();

        // Submit and wait for completion
        m_commandBuffer->waitUntilCompleted();

        auto endTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
        return elapsed.count();
    }

    void Rasterizer::updateCameraUniforms(const Camera &camera)
    {
        // View matrix (camera transform)
        m_viewMatrix = glm::lookAt(
            glm::vec3(camera.position().x, camera.position().y, camera.position().z),
            glm::vec3(camera.position().x + camera.forward().x,
                     camera.position().y + camera.forward().y,
                     camera.position().z + camera.forward().z),
            glm::vec3(camera.up().x, camera.up().y, camera.up().z)
        );

        // Projection matrix (flip Y for Vulkan coordinate system)
        m_projectionMatrix = glm::perspective(
            glm::radians(camera.fov()),
            camera.aspectRatio(),
            camera.nearPlane(),
            camera.farPlane()
        );
        m_projectionMatrix[1][1] *= -1.0f; // Flip Y-axis for Vulkan
    }

    void Rasterizer::renderScene(const SceneCompiler::CompiledScene &scene)
    {
        // Iterate through all instances and render their geometry
        for (size_t i = 0; i < scene.instances.size(); ++i)
        {
            const auto& instance = scene.instances[i];

            // Get BLAS index from instance (stored in blasAddress)
            size_t blasIndex = static_cast<size_t>(instance.blasAddress);

            // Safety check
            if (blasIndex >= scene.vertexBuffers.size())
            {
                continue; // Skip invalid instance
            }

            // Get vertex buffer and count for this BLAS
            const Buffer* vertexBuffer = scene.vertexBuffers[blasIndex].get();
            uint32_t vertexCount = scene.vertexCounts[blasIndex];

            // Extract model matrix from instance transform (3x4 row-major -> 4x4 column-major)
            glm::mat4 model(1.0f); // Identity
            for (int r = 0; r < 3; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    model[c][r] = instance.transform[r][c];
                }
            }
            // Last row is [0, 0, 0, 1] for homogeneous coordinates

            // Compute MVP matrix
            glm::mat4 mvp = m_projectionMatrix * m_viewMatrix * model;

            // Get material for this instance
            glm::vec4 baseColor(0.8f, 0.8f, 0.8f, 1.0f); // Default light gray
            if (i < scene.instanceToMaterialIndex.size())
            {
                uint32_t materialIndex = scene.instanceToMaterialIndex[i];
                if (materialIndex < scene.materials.size())
                {
                    const auto& material = scene.materials[materialIndex];
                    baseColor = glm::vec4(
                        material.baseColorR,
                        material.baseColorG,
                        material.baseColorB,
                        material.baseColorA
                    );
                }
            }

            // Bind vertex buffer
            m_commandBuffer->bindVertexBuffer(vertexBuffer, 0);

            // Push constants: MVP matrix (64 bytes) + base color (16 bytes) = 80 bytes
            struct PushConstants {
                glm::mat4 mvp;
                glm::vec4 baseColor;
            } pushConstants;
            pushConstants.mvp = mvp;
            pushConstants.baseColor = baseColor;

            m_commandBuffer->pushConstants(&pushConstants, sizeof(PushConstants), 0);

            // Draw vertices (non-indexed)
            m_commandBuffer->draw(vertexCount, 1, 0, 0);
        }
    }

    size_t Rasterizer::readback(void *outData)
    {
        if (!outData)
        {
            throw std::runtime_error("Rasterizer::readback: outData cannot be null");
        }

        // Copy image to readback buffer (using command buffer)
        m_commandBuffer->begin();
        m_commandBuffer->copyImageToBuffer(m_outputImage.get(), m_readbackBuffer.get());
        m_commandBuffer->end();
        m_commandBuffer->waitUntilCompleted();

        // Read from buffer to CPU memory
        const size_t pixelSize = 4; // R8G8B8A8
        const size_t bufferSize = m_config.width * m_config.height * pixelSize;

        const void *bufferData = m_readbackBuffer->mapForReading();
        std::memcpy(outData, bufferData, bufferSize);
        m_readbackBuffer->unmap();

        return bufferSize;
    }
} // namespace tracey
