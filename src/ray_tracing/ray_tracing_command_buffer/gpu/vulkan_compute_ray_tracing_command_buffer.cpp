#include "vulkan_compute_ray_tracing_command_buffer.hpp"
#include "../../../device/gpu/vulkan_compute_device.hpp"
#include "../../../ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_pipeline.hpp"
#include "../../../ray_tracing/ray_tracing_pipeline/gpu/wavefront/vulkan_wavefront_pipeline.hpp"
#include "../../../ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_descriptor_set.hpp"
#include "../../../device/gpu/vulkan_buffer.hpp"
#include "../../../device/gpu/vulkan_image_2d.hpp"
#include <iostream>

namespace tracey
{
    // ============================================================================
    // Constructor / Destructor
    // ============================================================================

    VulkanComputeRayTracingCommandBuffer::VulkanComputeRayTracingCommandBuffer(VulkanComputeDevice &device)
        : m_device(device)
    {
        m_vkCommandBuffer = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_device.commandPool();
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device.vkDevice(), &allocInfo, &m_vkCommandBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate Vulkan command buffer");
        }
    }

    VulkanComputeRayTracingCommandBuffer::~VulkanComputeRayTracingCommandBuffer()
    {
        if (m_vkCommandBuffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(m_device.vkDevice(), m_device.commandPool(), 1, &m_vkCommandBuffer);
        }
    }

    // ============================================================================
    // Public Interface
    // ============================================================================

    void VulkanComputeRayTracingCommandBuffer::begin()
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(m_vkCommandBuffer, &beginInfo);
    }

    void VulkanComputeRayTracingCommandBuffer::end()
    {
        vkEndCommandBuffer(m_vkCommandBuffer);

        // Submit command buffer to the compute queue
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_vkCommandBuffer;
        if (vkQueueSubmit(m_device.computeQueue(), 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to submit command buffer");
        }

        m_descriptorSets.clear();
    }

    void VulkanComputeRayTracingCommandBuffer::setPipeline(RayTracingPipeline *pipeline)
    {
        m_pipeline = pipeline;

        // Only bind pipeline now if it's monolithic
        // Wavefront pipelines bind individual stages in traceRays()
        if (auto *monolithic = dynamic_cast<VulkanComputeRaytracingPipeline *>(pipeline))
        {
            vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, monolithic->vkPipeline());
        }
    }

    void VulkanComputeRayTracingCommandBuffer::setDescriptorSet(DescriptorSet *set)
    {
        const auto vkDescriptorSet = dynamic_cast<VulkanComputeRayTracingDescriptorSet *>(set)->vkDescriptorSet();

        // For wavefront pipelines, store descriptor sets for later binding during bounce loop
        if (auto *wavefront = dynamic_cast<VulkanWaveFrontPipeline *>(m_pipeline))
        {
            m_descriptorSets.push_back(vkDescriptorSet);
        }
        else if (auto *monolithic = dynamic_cast<VulkanComputeRaytracingPipeline *>(m_pipeline))
        {
            // For monolithic pipeline, bind immediately
            VkPipelineLayout layout = monolithic->vkPipelineLayout();
            vkCmdBindDescriptorSets(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    layout, 0, 1, &vkDescriptorSet, 0, nullptr);
        }
        else
        {
            throw std::runtime_error("Unknown pipeline type");
        }
    }

    void VulkanComputeRayTracingCommandBuffer::traceRays(const ShaderBindingTable &sbt,
                                                         uint32_t width, uint32_t height,
                                                         const TraceRaysParams &params)
    {
        if (auto *wavefront = dynamic_cast<VulkanWaveFrontPipeline *>(m_pipeline))
        {
            // ================================================================
            // WAVEFRONT PATH TRACING EXECUTION
            // ================================================================
            // The wavefront algorithm processes rays in stages:
            // 1. Ray Generation: spawn primary rays from camera
            // 2. For each bounce:
            //    a. Intersection: test rays against scene geometry
            //    b. Hit Shader: process hits, spawn secondary rays
            //    c. Miss Shader: process misses (sky/environment)
            // 3. Resolve: accumulate final colors to output image
            // ================================================================

            const uint32_t sampleCount = params.samplesPerFrame;
            const uint32_t maxBounces = params.maxBounces;
            uint32_t rayCount = width * height;
            uint32_t workGroups = (rayCount + 255) / 256;

            WavefrontPushConstants pushConstants{width, height, 0u, sampleCount};

            // Initialize buffers once at the start
            initializeWavefrontBuffers(wavefront);

            // Multi-sample loop for anti-aliasing / Monte Carlo integration
            for (size_t sample = 0; sample < sampleCount; ++sample)
            {
                pushConstants.sampleIndex = static_cast<uint32_t>(sample);

                // Reset hit info for this sample
                clearHitInfoBuffer(wavefront);

                // Clear ray queue counter before ray generation (ray gen uses atomicAdd)
                clearRayQueueForRayGen(wavefront);

                // Bind descriptor set 0 for ray generation
                if (!m_descriptorSets.empty())
                {
                    vkCmdBindDescriptorSets(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            wavefront->pipelineLayout(), 0, 1,
                                            &m_descriptorSets[0], 0, nullptr);
                }

                // Spawn primary rays
                dispatchRayGeneration(wavefront, pushConstants, workGroups);
                insertComputeBarrier();

                // DEBUG: Log execution flow (only for sample 0)
                if (sample == 0)
                {
                    std::cout << "[DEBUG] After ray gen: dispatched " << workGroups << " workgroups for "
                              << rayCount << " rays" << std::endl;
                    std::cout << "[DEBUG] Descriptor sets available: " << m_descriptorSets.size() << std::endl;
                }

                // Bounce loop: trace rays through the scene
                for (uint32_t bounce = 0; bounce < maxBounces; bounce++)
                {
                    // Ping-pong between descriptor sets for double-buffered ray queues
                    uint32_t currentSetIndex = bounce % 2;
                    if (!m_descriptorSets.empty() && m_descriptorSets.size() > currentSetIndex)
                    {
                        vkCmdBindDescriptorSets(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                wavefront->pipelineLayout(), 0, 1,
                                                &m_descriptorSets[currentSetIndex], 0, nullptr);
                    }

                    // Clear queues for this bounce iteration
                    clearBounceBuffers(wavefront, bounce);

                    // DEBUG: Log execution flow (only for sample 0, only first 2 bounces)
                    if (sample == 0 && bounce < 2)
                    {
                        const char *currentBufferName = (bounce % 2 == 0) ? "rayQueueBuffer" : "rayQueueBuffer2";
                        const char *nextBufferName = (bounce % 2 == 0) ? "rayQueueBuffer2" : "rayQueueBuffer";
                        std::cout << "[DEBUG] Bounce " << bounce << ": descriptor set " << (bounce % 2)
                                  << ", current buffer = " << currentBufferName
                                  << ", next buffer = " << nextBufferName << std::endl;
                    }

                    // Compute workgroup counts for indirect dispatch
                    dispatchPrepareIndirect(wavefront);
                    insertIndirectDispatchBarrier();

                    // Test rays against scene geometry
                    dispatchIntersection(wavefront, pushConstants, workGroups);
                    insertComputeBarrier();

                    // Recompute indirect args now that hit/miss queues are populated
                    dispatchPrepareIndirect(wavefront);

                    // Need BOTH indirect dispatch barrier AND compute barrier
                    // - Indirect barrier: for vkCmdDispatchIndirect to read the workgroup counts
                    // - Compute barrier: for hit/miss shaders to read queue counts and indices
                    insertIndirectDispatchBarrier();
                    insertComputeBarrier();

                    // Bin hitQueue by material program ID so each workgroup
                    // in the hit shader runs lanes with the same VM opcode
                    // stream (SIMT coherence on the interpreter).
                    dispatchSortHitQueue(wavefront);
                    insertComputeBarrier();

                    // Process hits and misses
                    dispatchHitShader(wavefront, pushConstants);
                    insertComputeBarrier();

                    dispatchMissShader(wavefront, pushConstants);
                    insertComputeBarrier();
                }

                // Write accumulated color to output image
                dispatchResolve(wavefront, pushConstants, width, height);
            }
        }
        else
        {
            // ================================================================
            // MONOLITHIC PIPELINE (legacy single-dispatch approach)
            // ================================================================
            vkCmdDispatch(m_vkCommandBuffer, width / 32, height / 32, 1);
        }
    }

    void VulkanComputeRayTracingCommandBuffer::copyImageToBuffer(const Image2D *image, Buffer *buffer)
    {
        // Transition image layout from GENERAL to TRANSFER_SRC_OPTIMAL
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = dynamic_cast<const VulkanImage2D *>(image)->vkImage();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(
            m_vkCommandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier);

        // Copy image contents to staging buffer
        VkBufferImageCopy copyRegion{};
        const VulkanImage2D *vulkanImage = dynamic_cast<const VulkanImage2D *>(image);
        copyRegion.imageExtent.width = vulkanImage->width();
        copyRegion.imageExtent.height = vulkanImage->height();
        copyRegion.imageExtent.depth = 1;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;

        vkCmdCopyImageToBuffer(
            m_vkCommandBuffer,
            dynamic_cast<const VulkanImage2D *>(image)->vkImage(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dynamic_cast<VulkanBuffer *>(buffer)->vkBuffer(),
            1,
            &copyRegion);

        // Transition image back to GENERAL for subsequent compute shader use
        VkImageMemoryBarrier barrierBack{};
        barrierBack.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrierBack.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrierBack.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrierBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrierBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrierBack.image = dynamic_cast<const VulkanImage2D *>(image)->vkImage();
        barrierBack.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrierBack.subresourceRange.baseMipLevel = 0;
        barrierBack.subresourceRange.levelCount = 1;
        barrierBack.subresourceRange.baseArrayLayer = 0;
        barrierBack.subresourceRange.layerCount = 1;
        barrierBack.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrierBack.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        vkCmdPipelineBarrier(
            m_vkCommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrierBack);
    }

    void VulkanComputeRayTracingCommandBuffer::waitUntilCompleted()
    {
        vkQueueWaitIdle(m_device.computeQueue());
    }

    void VulkanComputeRayTracingCommandBuffer::clearImage(Image2D *image, float r, float g, float b, float a)
    {
        VkClearColorValue clearColor = {{r, g, b, a}};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        vkCmdClearColorImage(
            m_vkCommandBuffer,
            dynamic_cast<VulkanImage2D *>(image)->vkImage(),
            VK_IMAGE_LAYOUT_GENERAL,
            &clearColor,
            1,
            &range);

        // Barrier to ensure clear completes before shader access
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        vkCmdPipelineBarrier(
            m_vkCommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // ============================================================================
    // Wavefront Pipeline Helpers - Buffer Initialization
    // ============================================================================

    void VulkanComputeRayTracingCommandBuffer::initializeWavefrontBuffers(VulkanWaveFrontPipeline *wavefront)
    {
        // Zero-initialize payload buffer (ray payloads start with default values)
        VkBuffer payloadBuf = wavefront->payloadBuffer();
        if (payloadBuf != VK_NULL_HANDLE)
        {
            vkCmdFillBuffer(m_vkCommandBuffer, payloadBuf, 0, VK_WHOLE_SIZE, 0);
            insertTransferToComputeBarrier();
        }
    }

    void VulkanComputeRayTracingCommandBuffer::clearHitInfoBuffer(VulkanWaveFrontPipeline *wavefront)
    {
        // Set all triangle indices to 0xFFFFFFFF (invalid/no hit)
        VkBuffer hitInfoBuf = wavefront->hitInfoBuffer();
        if (hitInfoBuf != VK_NULL_HANDLE)
        {
            vkCmdFillBuffer(m_vkCommandBuffer, hitInfoBuf, 0, VK_WHOLE_SIZE, 0xFFFFFFFF);
            insertTransferToComputeBarrier();
        }
    }

    void VulkanComputeRayTracingCommandBuffer::clearRayQueueForRayGen(VulkanWaveFrontPipeline *wavefront)
    {
        // Clear BOTH ray queue counters to 0 before ray generation.
        // Ray gen uses atomicAdd to increment count, so it must start at 0.
        // We clear both to ensure clean state for the new sample.
        vkCmdFillBuffer(m_vkCommandBuffer, wavefront->rayQueueBuffer(), 0, sizeof(uint32_t), 0);
        vkCmdFillBuffer(m_vkCommandBuffer, wavefront->rayQueueBuffer2(), 0, sizeof(uint32_t), 0);

        // Also clear hit and miss queue counters to ensure clean state
        vkCmdFillBuffer(m_vkCommandBuffer, wavefront->hitQueueBuffer(), 0, sizeof(uint32_t), 0);
        vkCmdFillBuffer(m_vkCommandBuffer, wavefront->missQueueBuffer(), 0, sizeof(uint32_t), 0);

        insertTransferToComputeBarrier();
    }

    void VulkanComputeRayTracingCommandBuffer::clearBounceBuffers(VulkanWaveFrontPipeline *wavefront,
                                                                  uint32_t bounce)
    {
        // Clear the "next" ray queue counter (ping-pong based on bounce index)
        VkBuffer nextQueueBuffer = (bounce % 2 == 0)
                                       ? wavefront->rayQueueBuffer2()
                                       : wavefront->rayQueueBuffer();
        vkCmdFillBuffer(m_vkCommandBuffer, nextQueueBuffer, 0, sizeof(uint32_t), 0);

        // Clear hit and miss queue counters
        vkCmdFillBuffer(m_vkCommandBuffer, wavefront->hitQueueBuffer(), 0, sizeof(uint32_t), 0);
        vkCmdFillBuffer(m_vkCommandBuffer, wavefront->missQueueBuffer(), 0, sizeof(uint32_t), 0);

        insertTransferToComputeBarrier();

        // Clear hit info buffer for fresh intersection results
        vkCmdFillBuffer(m_vkCommandBuffer, wavefront->hitInfoBuffer(), 0, VK_WHOLE_SIZE, 0xFFFFFFFF);
        insertTransferToComputeBarrier();
    }

    // ============================================================================
    // Wavefront Pipeline Helpers - Shader Dispatch
    // ============================================================================

    void VulkanComputeRayTracingCommandBuffer::dispatchRayGeneration(VulkanWaveFrontPipeline *wavefront,
                                                                     const WavefrontPushConstants &pushConstants,
                                                                     uint32_t workGroups)
    {
        vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          wavefront->rayGenPipeline());
        vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                           &pushConstants);
        vkCmdDispatch(m_vkCommandBuffer, workGroups, 1, 1);
    }

    void VulkanComputeRayTracingCommandBuffer::dispatchPrepareIndirect(VulkanWaveFrontPipeline *wavefront)
    {
        // Single-threaded shader that reads queue counts and computes workgroup sizes
        vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          wavefront->prepareIndirectPipeline());
        vkCmdDispatch(m_vkCommandBuffer, 1, 1, 1);
    }

    void VulkanComputeRayTracingCommandBuffer::dispatchIntersection(VulkanWaveFrontPipeline *wavefront,
                                                                    const WavefrontPushConstants &pushConstants,
                                                                    uint32_t workGroups)
    {
        (void)workGroups; // Unused - we use indirect dispatch based on ray queue count
        vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          wavefront->intersectPipeline());
        vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                           &pushConstants);
        // Use indirect dispatch based on rayQueue.count (computed by prepareIndirect)
        vkCmdDispatchIndirect(m_vkCommandBuffer, wavefront->indirectDispatchBuffer(), 0);
    }

    void VulkanComputeRayTracingCommandBuffer::dispatchSortHitQueue(VulkanWaveFrontPipeline *wavefront)
    {
        // Pass 1: count rays per bin and scan to offsets. Single 64-thread
        // workgroup so the scan stays in shared memory. The kernel itself
        // resets the materialBinCursors and writes sortedHitQueue.count, so
        // we don't need a separate clear here.
        vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          wavefront->sortCountPipeline());
        vkCmdDispatch(m_vkCommandBuffer, 1, 1, 1);

        insertComputeBarrier();

        // Pass 2: scatter. Indirect dispatch using the hit indirect buffer
        // (workgroup count == ceil(hitQueue.count / 256), which is exactly
        // what prepareIndirect already wrote there).
        vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          wavefront->sortScatterPipeline());
        vkCmdDispatchIndirect(m_vkCommandBuffer, wavefront->hitIndirectBuffer(), 0);
    }

    void VulkanComputeRayTracingCommandBuffer::dispatchHitShader(VulkanWaveFrontPipeline *wavefront,
                                                                 const WavefrontPushConstants &pushConstants)
    {
        vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          wavefront->hitPipeline(0));
        vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                           &pushConstants);
        // Indirect dispatch based on hit queue count
        vkCmdDispatchIndirect(m_vkCommandBuffer, wavefront->hitIndirectBuffer(), 0);
    }

    void VulkanComputeRayTracingCommandBuffer::dispatchMissShader(VulkanWaveFrontPipeline *wavefront,
                                                                  const WavefrontPushConstants &pushConstants)
    {
        vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          wavefront->missPipeline(0));
        vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                           &pushConstants);
        // Indirect dispatch based on miss queue count
        vkCmdDispatchIndirect(m_vkCommandBuffer, wavefront->missIndirectBuffer(), 0);
    }

    void VulkanComputeRayTracingCommandBuffer::dispatchResolve(VulkanWaveFrontPipeline *wavefront,
                                                               const WavefrontPushConstants &pushConstants,
                                                               uint32_t width, uint32_t height)
    {
        vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          wavefront->resolvePipeline());
        vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                           &pushConstants);

        // Resolve uses 16x16 workgroups for 2D dispatch
        uint32_t resolveWorkGroupsX = (width + 15) / 16;
        uint32_t resolveWorkGroupsY = (height + 15) / 16;
        vkCmdDispatch(m_vkCommandBuffer, resolveWorkGroupsX, resolveWorkGroupsY, 1);
    }

    // ============================================================================
    // Wavefront Pipeline Helpers - Synchronization Barriers
    // ============================================================================

    void VulkanComputeRayTracingCommandBuffer::insertComputeBarrier()
    {
        // TEST: Full pipeline barrier to rule out sync issues
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        vkCmdPipelineBarrier(m_vkCommandBuffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    void VulkanComputeRayTracingCommandBuffer::insertTransferToComputeBarrier()
    {
        // TEST: Full pipeline barrier to rule out sync issues
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        vkCmdPipelineBarrier(m_vkCommandBuffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    void VulkanComputeRayTracingCommandBuffer::insertIndirectDispatchBarrier()
    {
        // TEST: Full pipeline barrier to rule out sync issues
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        vkCmdPipelineBarrier(m_vkCommandBuffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

} // namespace tracey
