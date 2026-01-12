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
        VkPipelineLayout layout;

        // Get pipeline layout from either monolithic or wavefront pipeline
        if (auto *monolithic = dynamic_cast<VulkanComputeRaytracingPipeline *>(m_pipeline))
        {
            layout = monolithic->vkPipelineLayout();
        }
        else if (auto *wavefront = dynamic_cast<VulkanWaveFrontPipeline *>(m_pipeline))
        {
            layout = wavefront->pipelineLayout();
        }
        else
        {
            throw std::runtime_error("Unknown pipeline type");
        }

        const auto vkDescriptorSet = dynamic_cast<VulkanComputeRayTracingDescriptorSet *>(set)->vkDescriptorSet();
        vkCmdBindDescriptorSets(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &vkDescriptorSet, 0, nullptr);
    }
    void VulkanComputeRayTracingCommandBuffer::traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height)
    {
        // Check if wavefront pipeline
        if (auto *wavefront = dynamic_cast<VulkanWaveFrontPipeline *>(m_pipeline))
        {
            // === WAVEFRONT MULTI-STAGE EXECUTION ===

            uint32_t rayCount = width * height;
            uint32_t workGroups = (rayCount + 255) / 256; // 256 threads per work group

            // Push constants (resolution)
            struct PushConstants
            {
                uint32_t width;
                uint32_t height;
            } pushConstants{width, height};

            // Stage 1: Ray Generation
            vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              wavefront->rayGenPipeline());
            vkCmdPushConstants(m_vkCommandBuffer, wavefront->pipelineLayout(),
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                               &pushConstants);
            vkCmdDispatch(m_vkCommandBuffer, workGroups, 1, 1);

            // Barrier: Wait for ray gen to complete before intersection
            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(m_vkCommandBuffer,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);

            // Stage 2: Intersection (BVH traversal)
            vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              wavefront->intersectPipeline());
            vkCmdDispatch(m_vkCommandBuffer, workGroups, 1, 1);

            // Barrier: Wait for intersection before hit/miss shaders
            vkCmdPipelineBarrier(m_vkCommandBuffer,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);

            // Stage 3: Hit shader (simplified - just use first hit shader for all hits)
            vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              wavefront->hitPipeline(0));
            vkCmdDispatch(m_vkCommandBuffer, workGroups, 1, 1);

            // Barrier after hit shader
            vkCmdPipelineBarrier(m_vkCommandBuffer,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);

            // Stage 4: Miss shader (simplified - just use first miss shader)
            vkCmdBindPipeline(m_vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              wavefront->missPipeline(0));
            vkCmdDispatch(m_vkCommandBuffer, workGroups, 1, 1);
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