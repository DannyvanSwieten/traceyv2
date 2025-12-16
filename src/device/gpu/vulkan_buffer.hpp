#pragma once

#include <vulkan/vulkan.h>
#include "../buffer.hpp"
#include "../device.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class VulkanBuffer : public Buffer
    {
    public:
        VulkanBuffer(VulkanComputeDevice &device, uint32_t size, BufferUsage usageFlags);
        ~VulkanBuffer();

        void *mapForWriting() override;
        const void *mapForReading() const override;
        void mapRange(uint32_t offset, uint32_t size) override;
        void flush() override;
        void flushRange(uint32_t offset, uint32_t size) override;

        VkBuffer vkBuffer() const { return m_buffer; }
        VkDeviceMemory vkDeviceMemory() const { return m_memory; }

    private:
        VkDevice m_device;
        VkBuffer m_buffer;
        VkDeviceMemory m_memory;
    };
} // namespace tracey