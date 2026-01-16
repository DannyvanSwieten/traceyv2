#include "vulkan_compute_ray_tracing_command_buffer.hpp"
#include "../../../device/gpu/vulkan_compute_device.hpp"
#include "../../../ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_pipeline.hpp"
#include "../../../ray_tracing/ray_tracing_pipeline/gpu/wavefront/vulkan_wavefront_pipeline.hpp"
#include "../../../ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_descriptor_set.hpp"
#include "../../../device/gpu/vulkan_buffer.hpp"
#include "../../../device/gpu/vulkan_image_2d.hpp"
namespace tracey
{
    VulkanComputeRayTracingCommandBuffer::VulkanComputeRayTracingCommandBuffer(VulkanComputeDevice &device) : m_device(device)
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
    void VulkanComputeRayTracingCommandBuffer::begin()
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(m_vkCommandBuffer, &beginInfo);
    }
    void VulkanComputeRayTracingCommandBuffer::end()
    {
        vkEndCommandBuffer(m_vkCommandBuffer);
        // Commit command buffer to the queue
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_vkCommandBuffer;
        if (vkQueueSubmit(m_device.computeQueue(), 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to submit command buffer");
        }
    }
    void VulkanComputeRayTracingCommandBuffer::setPipeline(RayTracingPipeline *pipeline)
    {
        m_pipeline = pipeline;

        // Only bind pipeline now if it's monolithic
        // Wavefront pipelines bind in traceRays() since we need multiple pipelines
        if (auto *monolithic = dynamic_cast<VulkanComputeRaytracingPipeline *>(pipeline))
        {
            vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, monolithic->vkPipeline());
        }
    }
    void VulkanComputeRayTracingCommandBuffer::setDescriptorSet(DescriptorSet *set)
    {
        const auto vkDescriptorSet = dynamic_cast<VulkanComputeRayTracingDescriptorSet *>(set)->vkDescriptorSet();

        // For wavefront pipelines, store descriptor sets instead of binding immediately
        // They will be bound during traceRays() for multi-bounce support
        if (auto *wavefront = dynamic_cast<VulkanWaveFrontPipeline *>(m_pipeline))
        {
            m_descriptorSets.push_back(vkDescriptorSet);
        }
        else if (auto *monolithic = dynamic_cast<VulkanComputeRaytracingPipeline *>(m_pipeline))
        {
            // For monolithic pipeline, bind immediately
            VkPipelineLayout layout = monolithic->vkPipelineLayout();
            vkCmdBindDescriptorSets(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &vkDescriptorSet, 0, nullptr);
        }
        else
        {
            throw std::runtime_error("Unknown pipeline type");
        }
    }
    void VulkanComputeRayTracingCommandBuffer::traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height)
    {
        // Check if wavefront pipeline
        if (auto *wavefront = dynamic_cast<VulkanWaveFrontPipeline *>(m_pipeline))
        {
            // === WAVEFRONT MULTI-BOUNCE EXECUTION ===

            const uint32_t maxBounces = 3; // TODO: Make configurable
            uint32_t rayCount = width * height;
            uint32_t workGroups = (rayCount + 255) / 256; // 256 threads per work group

            // Push constants (resolution)
            struct PushConstants
            {
                uint32_t width;
                uint32_t height;
            } pushConstants{width, height};

            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

            // Clear output image to black before rendering
            // Note: This requires accessing the output image from descriptor sets
            // For now, we'll rely on the application to clear the image before rendering
            // TODO: Add proper image clear command here if needed

            VkBuffer payloadBuf = wavefront->payloadBuffer();
            VkBuffer hitInfoBuf = wavefront->hitInfoBuffer();

            for (size_t sample = 0; sample < 4; ++sample) // Single sample for now
            {
                // Initialize payload buffer to zero for each sample (sets alive=false initially, ray_gen will set it to true)
                if (payloadBuf != VK_NULL_HANDLE)
                {
                    vkCmdFillBuffer(m_vkCommandBuffer, payloadBuf, 0, VK_WHOLE_SIZE, 0);

                    VkMemoryBarrier fillBarrier{};
                    fillBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &fillBarrier, 0, nullptr, 0, nullptr);
                }

                // Initialize HitInfo buffer with invalid triangle indices for each sample
                if (hitInfoBuf != VK_NULL_HANDLE)
                {
                    vkCmdFillBuffer(m_vkCommandBuffer, hitInfoBuf, 0, VK_WHOLE_SIZE, 0xFFFFFFFF);

                    VkMemoryBarrier fillBarrier{};
                    fillBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &fillBarrier, 0, nullptr, 0, nullptr);
                }

                // Bounce 0: Ray Generation (only happens once)
                uint32_t currentSetIndex = 0;

                if (!m_descriptorSets.empty())
                {
                    vkCmdBindDescriptorSets(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            wavefront->pipelineLayout(), 0, 1,
                                            &m_descriptorSets[currentSetIndex], 0, nullptr);
                }

                vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  wavefront->rayGenPipeline());
                vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                                   &pushConstants);
                vkCmdDispatch(m_vkCommandBuffer, workGroups, 1, 1);

                // Barrier to ensure ray_gen completes before intersection reads buffers
                VkMemoryBarrier raygenBarrier{};
                raygenBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                raygenBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                raygenBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(m_vkCommandBuffer,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0, 1, &raygenBarrier, 0, nullptr, 0, nullptr);

                // Dynamic bounce loop - continue until all rays are terminated
                const uint32_t MAX_ITERATIONS = 5;
                VkBuffer indirectDispatchBuffer = wavefront->indirectDispatchBuffer();

                // Barrier for indirect buffer reads
                VkMemoryBarrier indirectBarrier{};
                indirectBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                indirectBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                indirectBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;

                for (uint32_t bounce = 0; bounce < MAX_ITERATIONS; bounce++)
                {
                    currentSetIndex = bounce % 2;

                    // Bind current descriptor set (reads from binding 51, writes to binding 53)
                    if (!m_descriptorSets.empty() && m_descriptorSets.size() > currentSetIndex)
                    {
                        vkCmdBindDescriptorSets(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                wavefront->pipelineLayout(), 0, 1,
                                                &m_descriptorSets[currentSetIndex], 0, nullptr);
                    }

                    // Clear next queue count (binding 53) to 0
                    VkBuffer nextQueueBuffer = (bounce % 2 == 0) ? wavefront->rayQueueBuffer2() : wavefront->rayQueueBuffer();
                    vkCmdFillBuffer(m_vkCommandBuffer, nextQueueBuffer, 0, sizeof(uint32_t), 0);

                    // Clear hit queue count (binding 55) to 0
                    vkCmdFillBuffer(m_vkCommandBuffer, wavefront->hitQueueBuffer(), 0, sizeof(uint32_t), 0);

                    // Clear miss queue count (binding 56) to 0
                    vkCmdFillBuffer(m_vkCommandBuffer, wavefront->missQueueBuffer(), 0, sizeof(uint32_t), 0);

                    VkMemoryBarrier fillBarrier{};
                    fillBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

                    // Clear HitInfo buffer for this bounce
                    vkCmdFillBuffer(m_vkCommandBuffer, hitInfoBuf, 0, VK_WHOLE_SIZE, 0xFFFFFFFF);
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

                    // Prepare indirect dispatch arguments from current ray queue count
                    vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      wavefront->prepareIndirectPipeline());
                    vkCmdDispatch(m_vkCommandBuffer, 1, 1, 1);
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                         0, 1, &indirectBarrier, 0, nullptr, 0, nullptr);

                    // Intersection - DEBUG: use direct dispatch to ensure it runs
                    vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      wavefront->intersectPipeline());
                    vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                                       &pushConstants);
                    vkCmdDispatch(m_vkCommandBuffer, workGroups, 1, 1); // DEBUG: direct dispatch

                    // Stronger barrier to ensure hitQueue writes are visible
                    VkMemoryBarrier strongBarrier{};
                    strongBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    strongBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    strongBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_DEPENDENCY_BY_REGION_BIT, 1, &strongBarrier, 0, nullptr, 0, nullptr);

                    // Prepare hit/miss indirect dispatch arguments now that queues are populated
                    vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      wavefront->prepareIndirectPipeline());
                    vkCmdDispatch(m_vkCommandBuffer, 1, 1, 1);
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                         0, 1, &indirectBarrier, 0, nullptr, 0, nullptr);

                    // Hit shader - use indirect dispatch with hitIndirectBuffer
                    vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      wavefront->hitPipeline(0));
                    vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                                       &pushConstants);
                    vkCmdDispatchIndirect(m_vkCommandBuffer, wavefront->hitIndirectBuffer(), 0);
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &barrier, 0, nullptr, 0, nullptr);

                    // Miss shader - use indirect dispatch with missIndirectBuffer
                    vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      wavefront->missPipeline(0));
                    vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                                       &pushConstants);
                    vkCmdDispatchIndirect(m_vkCommandBuffer, wavefront->missIndirectBuffer(), 0);
                    vkCmdPipelineBarrier(m_vkCommandBuffer,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &barrier, 0, nullptr, 0, nullptr);
                }

                // Resolve shader (final pass)
                vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  wavefront->resolvePipeline());
                vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                                   &pushConstants);

                uint32_t resolveWorkGroupsX = (width + 15) / 16;
                uint32_t resolveWorkGroupsY = (height + 15) / 16;
                vkCmdDispatch(m_vkCommandBuffer, resolveWorkGroupsX, resolveWorkGroupsY, 1);
            }
        }
        else
        {
            // === MONOLITHIC PIPELINE (existing code) ===
            vkCmdDispatch(m_vkCommandBuffer, width / 32, height / 32, 1);
        }
    }
    void VulkanComputeRayTracingCommandBuffer::copyImageToBuffer(const Image2D *image, Buffer *buffer)
    {
        // First transition image layout to TRANSFER_SRC_OPTIMAL
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
    }
    void VulkanComputeRayTracingCommandBuffer::waitUntilCompleted()
    {
        vkQueueWaitIdle(m_device.computeQueue());
    }
} // namespace tracey