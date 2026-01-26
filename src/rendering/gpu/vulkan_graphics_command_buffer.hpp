#pragma once
#include <volk.h>
#include "../graphics_command_buffer.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class VulkanGraphicsPipeline;

    /// Vulkan implementation of graphics command buffer
    /// Records graphics rendering commands for execution on GPU
    class VulkanGraphicsCommandBuffer : public GraphicsCommandBuffer
    {
    public:
        VulkanGraphicsCommandBuffer(VulkanComputeDevice& device);
        ~VulkanGraphicsCommandBuffer() override;

        // GraphicsCommandBuffer interface
        void begin() override;
        void end() override;
        void beginRenderPass(GraphicsPipeline* pipeline,
                            float clearR, float clearG, float clearB, float clearA,
                            float clearDepth) override;
        void endRenderPass() override;
        void bindPipeline(GraphicsPipeline* pipeline) override;
        void bindVertexBuffer(const Buffer* buffer, uint32_t offset) override;
        void bindIndexBuffer(const Buffer* buffer, uint32_t offset) override;
        void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                        uint32_t firstIndex, int32_t vertexOffset,
                        uint32_t firstInstance) override;
        void draw(uint32_t vertexCount, uint32_t instanceCount,
                 uint32_t firstVertex, uint32_t firstInstance) override;
        void pushConstants(const void* data, uint32_t size, uint32_t offset) override;
        void bindDescriptorSet(DescriptorSet* set, uint32_t setIndex) override;
        void copyImageToBuffer(const Image2D* image, Buffer* buffer) override;
        void waitUntilCompleted() override;

        // Vulkan-specific accessor
        VkCommandBuffer vkCommandBuffer() const { return m_commandBuffer; }

    private:
        VulkanComputeDevice& m_device;
        VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
        GraphicsPipeline* m_currentPipeline = nullptr;
    };
}
