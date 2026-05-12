#include "rasterizer.hpp"
#include "../device/buffer.hpp"
#include "../device/gpu/vulkan_compute_device.hpp"
#include "gpu/vulkan_graphics_pipeline.hpp"
#include "gpu/vulkan_graphics_command_buffer.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>
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

    Image2D *Rasterizer::outputImage() const
    {
        return m_pipeline ? m_pipeline->colorTarget() : nullptr;
    }

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
        pipelineConfig.pointsVertexShader = m_config.pointsVertexShader;
        pipelineConfig.pointsFragmentShader = m_config.pointsFragmentShader;
        pipelineConfig.linesVertexShader = m_config.linesVertexShader;
        pipelineConfig.linesFragmentShader = m_config.linesFragmentShader;
        pipelineConfig.groundVertexShader = m_config.groundVertexShader;
        pipelineConfig.groundFragmentShader = m_config.groundFragmentShader;
        pipelineConfig.colorFormat = m_config.colorFormat;
        pipelineConfig.useDepthBuffer = m_config.useDepthBuffer;
        pipelineConfig.depthTestEnable = m_config.depthTestEnable;
        pipelineConfig.cullBackFaces = m_config.cullBackFaces;
        pipelineConfig.alphaBlending = m_config.alphaBlending;

        // The Device interface doesn't expose graphics-pipeline factories
        // (it's compute/ray-tracing first), so we construct the Vulkan
        // implementations directly. Requires a VulkanComputeDevice.
        auto* vulkanDevice = dynamic_cast<VulkanComputeDevice*>(m_device);
        if (!vulkanDevice)
        {
            throw std::runtime_error("Rasterizer: requires a VulkanComputeDevice");
        }

        m_pipeline = std::make_unique<VulkanGraphicsPipeline>(
            *vulkanDevice, pipelineConfig, *m_pipelineLayout);

        // The pipeline owns the color target; outputImage() is just a view.
        // Don't reset m_outputImage to take ownership — that double-frees.

        m_commandBuffer = std::make_unique<VulkanGraphicsCommandBuffer>(*vulkanDevice);

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
        // Optional reference ground grid. Drawn first so opaque scene geometry
        // overlays it; the ground pipeline has depth-write off so it doesn't
        // occlude anything below Y=0 either. Re-binds the main triangle
        // pipeline afterwards so the geometry loop has the right state.
        if (m_showGround && m_config.useDepthBuffer)
        {
            auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(m_pipeline.get());
            if (vkPipeline->hasGroundPipeline())
            {
                m_commandBuffer->bindGroundPipeline(m_pipeline.get());

                // Ground vertex shader procedurally emits 4 corners — no VB.
                // mvp uses the view*projection with identity model; we abuse
                // the existing baseColor push-constant slot to carry the
                // camera world position for the fragment shader's distance
                // fade.
                glm::mat4 vp = m_projectionMatrix * m_viewMatrix;
                // Reconstruct camera world position from the view matrix
                // (inverse[3]). Cheaper than re-plumbing the camera into here.
                glm::mat4 invView = glm::inverse(m_viewMatrix);
                struct PushConstants {
                    glm::mat4 mvp;
                    glm::vec4 baseColor;
                } pc;
                pc.mvp = vp;
                pc.baseColor = glm::vec4(invView[3][0], invView[3][1], invView[3][2], 1.0f);
                m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);
                m_commandBuffer->draw(4, 1, 0, 0);  // TRIANGLE_STRIP, 4 verts

                m_commandBuffer->bindPipeline(m_pipeline.get());
            }
        }

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
            const Buffer* vertexBuffer = scene.vertexBuffers[blasIndex];
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

            // Bind position (binding 0) + per-vertex Cd (binding 1). The
            // compiler always allocates a color buffer (default white) so the
            // pipeline's two-binding vertex input is always satisfied.
            m_commandBuffer->bindVertexBuffer(vertexBuffer, 0);
            if (blasIndex < scene.colorBuffers.size() && scene.colorBuffers[blasIndex])
            {
                m_commandBuffer->bindVertexBufferAt(
                    scene.colorBuffers[blasIndex], 1, 0);
            }

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

        // Optional wireframe overlay: bind the lines pipeline (POLYGON_MODE_LINE,
        // depth-test on, depth-write off, slight depth bias) and re-issue
        // draw calls against the same vertex buffers. Triangle topology stays
        // — only the rasterizer state differs.
        if (m_showEdges)
        {
            m_commandBuffer->bindLinesPipeline(m_pipeline.get());
            for (size_t i = 0; i < scene.instances.size(); ++i)
            {
                const auto& instance = scene.instances[i];
                size_t blasIndex = static_cast<size_t>(instance.blasAddress);
                if (blasIndex >= scene.vertexBuffers.size()) continue;
                const Buffer* vb = scene.vertexBuffers[blasIndex];
                uint32_t vertexCount = scene.vertexCounts[blasIndex];

                glm::mat4 model(1.0f);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        model[c][r] = instance.transform[r][c];

                glm::vec4 baseColor(0.8f, 0.8f, 0.8f, 1.0f);
                if (i < scene.instanceToMaterialIndex.size())
                {
                    uint32_t matIdx = scene.instanceToMaterialIndex[i];
                    if (matIdx < scene.materials.size())
                    {
                        const auto& m = scene.materials[matIdx];
                        baseColor = glm::vec4(m.baseColorR, m.baseColorG, m.baseColorB, m.baseColorA);
                    }
                }

                struct PushConstants {
                    glm::mat4 mvp;
                    glm::vec4 baseColor;
                } pc;
                pc.mvp = m_projectionMatrix * m_viewMatrix * model;
                pc.baseColor = baseColor;

                m_commandBuffer->bindVertexBuffer(vb, 0);
                m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);
                m_commandBuffer->draw(vertexCount, 1, 0, 0);
            }
        }

        // Optional points overlay: bind the points pipeline (sharing render
        // pass + framebuffer) and re-issue draw calls. The same vertex
        // buffers feed POINT_LIST topology, so each unique position becomes
        // a 2D point sprite drawn with alpha-blended circle splats.
        if (m_showPoints)
        {
            m_commandBuffer->bindPointsPipeline(m_pipeline.get());
            for (size_t i = 0; i < scene.instances.size(); ++i)
            {
                const auto& instance = scene.instances[i];
                size_t blasIndex = static_cast<size_t>(instance.blasAddress);
                if (blasIndex >= scene.vertexBuffers.size()) continue;
                const Buffer* vb = scene.vertexBuffers[blasIndex];
                uint32_t vertexCount = scene.vertexCounts[blasIndex];

                glm::mat4 model(1.0f);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        model[c][r] = instance.transform[r][c];

                glm::vec4 baseColor(0.8f, 0.8f, 0.8f, 1.0f);
                if (i < scene.instanceToMaterialIndex.size())
                {
                    uint32_t matIdx = scene.instanceToMaterialIndex[i];
                    if (matIdx < scene.materials.size())
                    {
                        const auto& m = scene.materials[matIdx];
                        baseColor = glm::vec4(m.baseColorR, m.baseColorG, m.baseColorB, m.baseColorA);
                    }
                }

                struct PushConstants {
                    glm::mat4 mvp;
                    glm::vec4 baseColor;
                } pc;
                pc.mvp = m_projectionMatrix * m_viewMatrix * model;
                pc.baseColor = baseColor;

                m_commandBuffer->bindVertexBuffer(vb, 0);
                m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);
                m_commandBuffer->draw(vertexCount, 1, 0, 0);
            }
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
        m_commandBuffer->copyImageToBuffer(outputImage(), m_readbackBuffer.get());
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
