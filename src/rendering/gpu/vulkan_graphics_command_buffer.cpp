#include "vulkan_graphics_command_buffer.hpp"
#include "vulkan_graphics_pipeline.hpp"
#include "../../device/gpu/vulkan_compute_device.hpp"
#include "../../device/gpu/vulkan_buffer.hpp"
#include "../../device/gpu/vulkan_image_2d.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/descriptor_set.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_descriptor_set.hpp"
#include <stdexcept>

namespace tracey
{
    VulkanGraphicsCommandBuffer::VulkanGraphicsCommandBuffer(VulkanComputeDevice& device)
        : m_device(device)
    {
        VkDevice vkDevice = m_device.vkDevice();

        // Allocate command buffer from device's command pool
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_device.commandPool();
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &m_commandBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate graphics command buffer");
        }
    }

    VulkanGraphicsCommandBuffer::~VulkanGraphicsCommandBuffer()
    {
        if (m_commandBuffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(m_device.vkDevice(), m_device.commandPool(), 1, &m_commandBuffer);
        }
    }

    void VulkanGraphicsCommandBuffer::begin()
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to begin recording graphics command buffer");
        }
    }

    void VulkanGraphicsCommandBuffer::end()
    {
        if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to end recording graphics command buffer");
        }
    }

    void VulkanGraphicsCommandBuffer::beginRenderPass(GraphicsPipeline* pipeline,
                                                      float clearR, float clearG, float clearB, float clearA,
                                                      float clearDepth)
    {
        m_currentPipeline = pipeline;
        auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(pipeline);

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = vkPipeline->vkRenderPass();
        renderPassInfo.framebuffer = vkPipeline->vkFramebuffer();
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {vkPipeline->config().width, vkPipeline->config().height};

        // Clear values
        std::vector<VkClearValue> clearValues;

        // Color clear value
        VkClearValue colorClear{};
        colorClear.color = {{clearR, clearG, clearB, clearA}};
        clearValues.push_back(colorClear);

        // Depth clear value (if depth buffer enabled)
        if (vkPipeline->config().useDepthBuffer)
        {
            VkClearValue depthClear{};
            depthClear.depthStencil = {clearDepth, 0};
            clearValues.push_back(depthClear);
        }

        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    }

    void VulkanGraphicsCommandBuffer::endRenderPass()
    {
        vkCmdEndRenderPass(m_commandBuffer);
    }

    void VulkanGraphicsCommandBuffer::bindPipeline(GraphicsPipeline* pipeline)
    {
        auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(pipeline);
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline->vkPipeline());
        m_currentPipeline = pipeline;
    }

    void VulkanGraphicsCommandBuffer::bindVertexBuffer(const Buffer* buffer, uint32_t offset)
    {
        auto* vkBuffer = static_cast<const VulkanBuffer*>(buffer);
        VkBuffer vertexBuffers[] = {vkBuffer->vkBuffer()};
        VkDeviceSize offsets[] = {offset};
        vkCmdBindVertexBuffers(m_commandBuffer, 0, 1, vertexBuffers, offsets);
    }

    void VulkanGraphicsCommandBuffer::bindIndexBuffer(const Buffer* buffer, uint32_t offset)
    {
        auto* vkBuffer = static_cast<const VulkanBuffer*>(buffer);
        vkCmdBindIndexBuffer(m_commandBuffer, vkBuffer->vkBuffer(), offset, VK_INDEX_TYPE_UINT32);
    }

    void VulkanGraphicsCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                                  uint32_t firstIndex, int32_t vertexOffset,
                                                  uint32_t firstInstance)
    {
        vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void VulkanGraphicsCommandBuffer::draw(uint32_t vertexCount, uint32_t instanceCount,
                                           uint32_t firstVertex, uint32_t firstInstance)
    {
        vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VulkanGraphicsCommandBuffer::pushConstants(const void* data, uint32_t size, uint32_t offset)
    {
        if (!m_currentPipeline)
        {
            throw std::runtime_error("Cannot push constants without a bound pipeline");
        }

        auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(m_currentPipeline);
        vkCmdPushConstants(m_commandBuffer, vkPipeline->vkPipelineLayout(),
                          VK_SHADER_STAGE_VERTEX_BIT, offset, size, data);
    }

    void VulkanGraphicsCommandBuffer::copyImageToBuffer(const Image2D* image, Buffer* buffer)
    {
        auto* vkImage = static_cast<const VulkanImage2D*>(image);
        auto* vkBuffer = static_cast<VulkanBuffer*>(buffer);

        // Transition image to transfer source layout
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vkImage->vkImage();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(m_commandBuffer,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy image to buffer
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {vkImage->width(), vkImage->height(), 1};

        vkCmdCopyImageToBuffer(m_commandBuffer, vkImage->vkImage(),
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              vkBuffer->vkBuffer(), 1, &region);

        // Transition image back to shader read layout
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(m_commandBuffer,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void VulkanGraphicsCommandBuffer::bindDescriptorSet(DescriptorSet* set, uint32_t setIndex)
    {
        if (!m_currentPipeline)
        {
            throw std::runtime_error("Cannot bind descriptor set without a bound pipeline");
        }

        auto* vkPipeline = static_cast<VulkanGraphicsPipeline*>(m_currentPipeline);
        auto* vkSet = static_cast<VulkanComputeRayTracingDescriptorSet*>(set);

        VkDescriptorSet vkDescriptorSet = vkSet->vkDescriptorSet();
        vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               vkPipeline->vkPipelineLayout(), setIndex, 1,
                               &vkDescriptorSet, 0, nullptr);
    }

    void VulkanGraphicsCommandBuffer::waitUntilCompleted()
    {
        VkDevice vkDevice = m_device.vkDevice();

        // Create fence for synchronization
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        VkFence fence;
        if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create fence");
        }

        // Submit command buffer to graphics queue
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_commandBuffer;

        // Use graphics queue for graphics commands
        // Note: VulkanComputeDevice needs to expose graphicsQueue()
        // For now, using compute queue (which should be unified with graphics)
        VkQueue queue = m_device.computeQueue();

        if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS)
        {
            vkDestroyFence(vkDevice, fence, nullptr);
            throw std::runtime_error("Failed to submit graphics command buffer");
        }

        // Wait for fence
        vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vkDevice, fence, nullptr);
    }
}
