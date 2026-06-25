#include "rasterizer.hpp"
#include "../core/parallel.hpp"
#include "../device/buffer.hpp"
#include "../device/gpu/vulkan_buffer.hpp"
#include "../device/gpu/vulkan_compute_device.hpp"
#include "../gpu/vulkan_queue_sync.hpp"
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

    Rasterizer::~Rasterizer()
    {
        // Tear down descriptor pool before m_pipeline (which owns the
        // descriptor-set layout). The destructor's default order
        // destroys m_pipeline first, which would invalidate the layout
        // while our pool's set is still alive. Explicit cleanup here
        // sequences correctly.
        if (m_lightsDescriptorPool != VK_NULL_HANDLE)
        {
            auto* vulkanDevice = dynamic_cast<VulkanComputeDevice*>(m_device);
            if (vulkanDevice)
            {
                vkDestroyDescriptorPool(vulkanDevice->vkDevice(),
                                        m_lightsDescriptorPool, nullptr);
            }
            m_lightsDescriptorPool = VK_NULL_HANDLE;
            m_lightsDescriptorSet  = VK_NULL_HANDLE;
        }
    }

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
        pipelineConfig.gizmoVertexShader = m_config.gizmoVertexShader;
        pipelineConfig.gizmoFragmentShader = m_config.gizmoFragmentShader;
        pipelineConfig.guidesVertexShader = m_config.guidesVertexShader;
        pipelineConfig.guidesFragmentShader = m_config.guidesFragmentShader;
        pipelineConfig.bonesVertexShader = m_config.bonesVertexShader;
        pipelineConfig.bonesFragmentShader = m_config.bonesFragmentShader;
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

        // One descriptor pool + one set for the lights SSBO. Pool sized
        // for exactly one storage-buffer descriptor; we never grow it
        // because the triangle pipeline's descriptor set is single-slot.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.maxSets       = 1;

        VkDevice vkDev = vulkanDevice->vkDevice();
        if (vkCreateDescriptorPool(vkDev, &poolInfo, nullptr,
                                   &m_lightsDescriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Rasterizer: failed to create lights descriptor pool");
        }

        auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(m_pipeline.get());
        VkDescriptorSetLayout layout = vkPipeline->vkDescriptorSetLayout();

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_lightsDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &layout;
        if (vkAllocateDescriptorSets(vkDev, &allocInfo,
                                     &m_lightsDescriptorSet) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Rasterizer: failed to allocate lights descriptor set");
        }
    }

    double Rasterizer::render(const SceneCompiler::CompiledScene &scene,
                              const Camera &camera)
    {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Free last render's per-instance batch buffers. We can't drop
        // them until the GPU has drained the previous dispatch, which
        // already happened in the prior render()'s waitForCompletion().
        m_transientInstanceBuffers.clear();

        // Record + submit happen under the process-wide Vulkan queue
        // mutex (every command-buffer call below touches the shared
        // command pool, which Vulkan requires to be externally
        // synchronised — without it, the cook worker's compute
        // dispatchers racing against the rasterizer trip
        // "THREADING ERROR: VkCommandPool simultaneously used").
        //
        // The fence wait is INTENTIONALLY outside the lock: fences
        // don't need queue/command-pool exclusion (they only read
        // fence state). Releasing the lock here lets the main-thread
        // present submit its own commands while this dispatch is
        // still running on the GPU — the whole point of moving the
        // rasterizer off the main thread.
        {
            std::lock_guard<std::mutex> gpuLock(vulkanQueueMutex());

            m_commandBuffer->begin();

            // Begin render pass with the configured background color.
            // Driven by RenderEngine::set_rasterizer_background_color so
            // the editor toolbar can offer preset / picker controls
            // without rebuilding the pipeline.
            m_commandBuffer->beginRenderPass(
                m_pipeline.get(),
                m_clearR, m_clearG, m_clearB, m_clearA,
                1.0f  // clear depth
            );

            m_commandBuffer->bindPipeline(m_pipeline.get());

            // Refresh the lights descriptor set if the engine's
            // lightBuffer has changed under us (compile_scene replaces
            // the buffer when the light count flips up/down). Cheaper
            // than re-writing the descriptor every frame; the buffer
            // identity is what changes, not its contents — and the
            // engine maps + flushes the same buffer in place when only
            // the data changed.
            if (scene.lightBuffer && scene.lightBuffer.get() != m_lastBoundLightBuffer)
            {
                auto* vulkanDevice = dynamic_cast<VulkanComputeDevice*>(m_device);
                auto* vkBuf = static_cast<VulkanBuffer*>(scene.lightBuffer.get());
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = vkBuf->vkBuffer();
                bufInfo.offset = 0;
                bufInfo.range  = VK_WHOLE_SIZE;
                VkWriteDescriptorSet write{};
                write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet          = m_lightsDescriptorSet;
                write.dstBinding      = 0;
                write.descriptorCount = 1;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo     = &bufInfo;
                vkUpdateDescriptorSets(vulkanDevice->vkDevice(), 1, &write, 0, nullptr);
                m_lastBoundLightBuffer = scene.lightBuffer.get();
            }

            // Bind the lights descriptor set so the fragment shader's
            // set-0 binding-0 storage-buffer reads land in a valid
            // memory region. Safe to call even with zero lights — the
            // engine always allocates at least one dummy entry.
            if (m_lightsDescriptorSet != VK_NULL_HANDLE && scene.lightBuffer)
            {
                auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(m_pipeline.get());
                auto* vkCb = static_cast<VulkanGraphicsCommandBuffer*>(m_commandBuffer.get());
                vkCmdBindDescriptorSets(vkCb->vkCommandBuffer(),
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vkPipeline->vkPipelineLayout(),
                                        0, 1, &m_lightsDescriptorSet, 0, nullptr);
            }

            updateCameraUniforms(camera);
            m_currentLightCount = scene.lightCount;
            renderScene(scene);

            m_commandBuffer->endRenderPass();
            m_commandBuffer->end();

            m_commandBuffer->submit();
        }

        m_commandBuffer->waitForCompletion();

        auto endTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
        return elapsed.count();
    }

    void Rasterizer::updateCameraUniforms(const Camera &camera)
    {
        m_cameraWorldPos = glm::vec3(camera.position().x, camera.position().y, camera.position().z);
        // View matrix (camera transform)
        m_viewMatrix = glm::lookAt(
            m_cameraWorldPos,
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

        // Batched draw path. Previously this loop issued one vkCmdDraw +
        // pushConstants per scene-instance, which at ~3000 particles
        // dominated tick_ms even though every draw was the same sphere.
        //
        // Now we group consecutive instances sharing a blasIndex into one
        // batch, pack their per-instance data (mat4 model + vec4 color)
        // into a transient INPUT_RATE_INSTANCE vertex buffer, and call
        // vkCmdDraw(vertexCount, instanceCount=N, …) once per batch.
        // Position + Cd are bound from the shared BLAS buffers; viewProj
        // goes through the push-constant slot.
        //
        // Push constants are 96 bytes total: mat4 viewProj + vec4
        // viewPos + uvec4 misc (.x = lightCount, .yzw padding).
        // viewPos.xyz is read by the fragment shader to compute view
        // direction for the BRDF; misc.x lets the shader loop over
        // [0, lightCount) without a separate buffer-size descriptor.
        // Layout must match the shader's PushConstants block AND the
        // 96-byte range declared in vulkan_graphics_pipeline.cpp.
        struct RasterPushConstants {
            glm::mat4  viewProj;
            glm::vec4  viewPos;
            glm::uvec4 misc;
        };
        RasterPushConstants pc{};
        pc.viewProj = m_projectionMatrix * m_viewMatrix;
        pc.viewPos  = glm::vec4(m_cameraWorldPos, 1.0f);
        pc.misc     = glm::uvec4(m_currentLightCount, 0u, 0u, 0u);
        m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);

        // Per-instance layout (must match the vertex input description
        // in vulkan_graphics_pipeline.cpp + the input declarations in
        // position_only.vert): mat4 model + albedo + (metallic,
        // roughness, emission strength) + emissive colour = 7 vec4.
        struct InstanceData {
            glm::mat4 model;
            glm::vec4 albedo;
            glm::vec4 mrx;       // x=metallic, y=roughness, z=emission strength
            glm::vec4 emissive;  // xyz = emission colour
        };
        static_assert(sizeof(InstanceData) == 112, "InstanceData layout must match shader binding");

        // Helper: pull a batch's per-instance data, allocate a transient
        // vertex buffer, upload, bind, and draw once.
        auto drawBatch = [&](size_t blasIndex, size_t startInst, size_t count) {
            if (count == 0 || blasIndex >= scene.vertexBuffers.size()) return;
            const Buffer *vb = scene.vertexBuffers[blasIndex];
            const uint32_t vertexCount = scene.vertexCounts[blasIndex];
            if (!vb || vertexCount == 0) return;

            // Build the per-instance buffer in parallel — every entry
            // touches only its own index, so parallel_for_chunks across
            // hardware_concurrency workers is safe. At 400k particles
            // the serial build was ~5–10 ms; the parallel version
            // drops it to ~1 ms on M3 Ultra. The lambda below is the
            // body of the original serial loop, unchanged.
            std::vector<InstanceData> insts(count);
            tracey::parallel_for_chunks(count,
                [&insts, &scene, startInst](size_t kBegin, size_t kEnd) {
                    for (size_t k = kBegin; k < kEnd; ++k)
                    {
                        const auto &inst = scene.instances[startInst + k];
                        glm::mat4 model(1.0f);
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 4; ++c)
                                model[c][r] = inst.transform[r][c];
                        glm::vec4 albedo(0.8f, 0.8f, 0.8f, 1.0f);
                        glm::vec4 mrx(0.0f, 0.5f, 0.0f, 0.0f);  // metallic, roughness, emissive strength
                        glm::vec4 emissive(0.0f, 0.0f, 0.0f, 0.0f);
                        const size_t mi = startInst + k;
                        if (mi < scene.instanceToMaterialIndex.size())
                        {
                            const uint32_t matIdx = scene.instanceToMaterialIndex[mi];
                            if (matIdx < scene.materials.size())
                            {
                                const auto &m = scene.materials[matIdx];
                                albedo  = glm::vec4(m.baseColorR, m.baseColorG, m.baseColorB, m.baseColorA);
                                mrx.x   = m.metallicFactor;
                                mrx.y   = m.roughnessFactor;
                                // emissive on the GPUMaterial is split
                                // across two struct slots — recombine.
                                emissive = glm::vec4(m.emissiveR, m.emissiveG, m.emissiveB, 0.0f);
                                // Use a strength of 1 when any channel is
                                // set; the frag shader multiplies these.
                                mrx.z   = (m.emissiveR + m.emissiveG + m.emissiveB) > 0.0f ? 1.0f : 0.0f;
                            }
                        }
                        insts[k].model    = model;
                        insts[k].albedo   = albedo;
                        insts[k].mrx      = mrx;
                        insts[k].emissive = emissive;
                    }
                });

            const uint32_t bytes = static_cast<uint32_t>(insts.size() * sizeof(InstanceData));
            auto instBuf = std::unique_ptr<Buffer>(
                m_device->createBuffer(bytes, BufferUsage::VertexBuffer));
            std::memcpy(instBuf->mapForWriting(), insts.data(), bytes);
            instBuf->flush();

            m_commandBuffer->bindVertexBuffer(vb, 0);
            if (blasIndex < scene.colorBuffers.size() && scene.colorBuffers[blasIndex])
            {
                m_commandBuffer->bindVertexBufferAt(
                    scene.colorBuffers[blasIndex], 1, 0);
            }
            m_commandBuffer->bindVertexBufferAt(instBuf.get(), 2, 0);
            m_commandBuffer->draw(vertexCount, static_cast<uint32_t>(count), 0, 0);

            // Keep the buffer alive until the GPU has consumed it. The
            // Rasterizer::render call doesn't return until
            // waitForCompletion(), so dropping the unique_ptr at end of
            // this lambda is safe — but we still need to keep it inside
            // the lambda body. Move it into a per-render keepalive list
            // so it survives across batches.
            m_transientInstanceBuffers.push_back(std::move(instBuf));
        };

        // Single linear scan grouping consecutive instances by blasIndex.
        // compile_scene emits instances in actor order, and each actor's
        // SceneInstances share the same BLAS (instance group case) or
        // are a single entry — so consecutive runs are exactly the
        // batchable chunks. Non-contiguous repeats are rare in practice
        // and just produce more, smaller batches.
        size_t i = 0;
        while (i < scene.instances.size())
        {
            const size_t blasIndex = static_cast<size_t>(scene.instances[i].blasAddress);
            size_t j = i + 1;
            while (j < scene.instances.size() &&
                   static_cast<size_t>(scene.instances[j].blasAddress) == blasIndex)
                ++j;
            drawBatch(blasIndex, i, j - i);
            i = j;
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
                // An object can have a BLAS but a null rasterizer vertex buffer
                // (the index-contract nullptr in compile_scene). Skip it —
                // binding a null buffer is a Vulkan error (drawBatch guards the
                // same way). Surfaced by instanced USD imports.
                if (!vb || vertexCount == 0) continue;

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
                // An object can have a BLAS but a null rasterizer vertex buffer
                // (the index-contract nullptr in compile_scene). Skip it —
                // binding a null buffer is a Vulkan error (drawBatch guards the
                // same way). Surfaced by instanced USD imports.
                if (!vb || vertexCount == 0) continue;

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
                // Bind Cd at binding 1 so the points vertex shader can
                // tint each point by its per-vertex colour. Mirrors the
                // triangle draw above. SceneCompiler always allocates a
                // colorBuffer (defaulting to white when Cd is missing)
                // so this binding is always safe.
                if (blasIndex < scene.colorBuffers.size() && scene.colorBuffers[blasIndex])
                {
                    m_commandBuffer->bindVertexBufferAt(
                        scene.colorBuffers[blasIndex], 1, 0);
                }
                m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);
                m_commandBuffer->draw(vertexCount, 1, 0, 0);
            }
        }

        // Translate-gizmo overlay. Drawn last so the depth-test-OFF
        // pipeline reads on top of everything (including PiP path-tracer
        // composite — the gizmo lives entirely in the rasterizer view).
        // Anchor + length are pushed via the standard PushConstants slot
        // shared with ground/points/lines.
        if (m_gizmoVisible)
        {
            auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(m_pipeline.get());
            if (vkPipeline->hasGizmoPipeline())
            {
                m_commandBuffer->bindGizmoPipeline(m_pipeline.get());
                struct PushConstants {
                    glm::mat4 mvp;
                    glm::vec4 baseColor;
                } pc;
                pc.mvp = m_projectionMatrix * m_viewMatrix;
                pc.baseColor = glm::vec4(m_gizmoAnchor.x, m_gizmoAnchor.y,
                                          m_gizmoAnchor.z, m_gizmoLength);
                m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);
                // 6 vertices: three axis lines (2 verts each).
                m_commandBuffer->draw(6, 1, 0, 0);
                m_commandBuffer->bindPipeline(m_pipeline.get());
            }
        }

        // Composition guides (rule-of-thirds / safe-area) — drawn dead last in
        // NDC, on top of everything (geometry + PT composite), alpha-blended.
        // The vertex shader emits the lines procedurally from gl_VertexIndex;
        // we only push the guide kind and issue the matching vertex count.
        // mvp is unused by the guides shader (positions are already in NDC).
        if (m_guidesMode != 0)
        {
            auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(m_pipeline.get());
            if (vkPipeline->hasGuidesPipeline())
            {
                m_commandBuffer->bindGuidesPipeline(m_pipeline.get());
                struct PushConstants {
                    glm::mat4 mvp;
                    glm::vec4 baseColor;
                } pc;
                pc.mvp = glm::mat4(1.0f);
                if (m_guidesMode & 1) // rule of thirds: 4 lines
                {
                    pc.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                    m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);
                    m_commandBuffer->draw(8, 1, 0, 0);
                }
                if (m_guidesMode & 2) // safe areas: action + title rects, 8 lines
                {
                    pc.baseColor = glm::vec4(2.0f, 0.0f, 0.0f, 0.0f);
                    m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);
                    m_commandBuffer->draw(16, 1, 0, 0);
                }
                m_commandBuffer->bindPipeline(m_pipeline.get());
            }
        }

        // Skeleton overlay + picked-joint highlight — world-space LINE_LISTs
        // drawn last with the depth-test-OFF bones pipeline so they read on top
        // of the skinned mesh. The editor pushes the posed endpoints each frame;
        // here we upload them to transient vertex buffers and draw, the skeleton
        // in its color then the highlight in a distinct one.
        if (!m_boneSegments.empty() || !m_boneHighlight.empty())
        {
            auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(m_pipeline.get());
            if (vkPipeline->hasBonesPipeline())
            {
                m_commandBuffer->bindBonesPipeline(m_pipeline.get());
                struct PushConstants {
                    glm::mat4 mvp;
                    glm::vec4 baseColor;
                } pc;
                pc.mvp = m_projectionMatrix * m_viewMatrix;
                auto drawSet = [&](const std::vector<glm::vec3>& segs, const glm::vec3& col) {
                    if (segs.empty()) return;
                    const size_t bytes = segs.size() * sizeof(glm::vec3);
                    auto buf = std::unique_ptr<Buffer>(
                        m_device->createBuffer(bytes, BufferUsage::VertexBuffer));
                    std::memcpy(buf->mapForWriting(), segs.data(), bytes);
                    pc.baseColor = glm::vec4(col, 1.0f);
                    m_commandBuffer->pushConstants(&pc, sizeof(pc), 0);
                    m_commandBuffer->bindVertexBuffer(buf.get(), 0);
                    m_commandBuffer->draw(static_cast<uint32_t>(segs.size()), 1, 0, 0);
                    // Keep alive until the GPU drains this dispatch (render()
                    // fences before returning); cleared next render.
                    m_transientInstanceBuffers.push_back(std::move(buf));
                };
                drawSet(m_boneSegments, m_boneColor);
                drawSet(m_boneHighlight, m_boneHighlightColor);
                m_commandBuffer->bindPipeline(m_pipeline.get());
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
