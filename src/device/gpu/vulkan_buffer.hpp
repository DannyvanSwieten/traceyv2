#pragma once
#include <volk.h>
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

        VulkanBuffer(const VulkanBuffer &) = delete;
        VulkanBuffer &operator=(const VulkanBuffer &) = delete;

        void *mapForWriting() override;
        const void *mapForReading() const override;
        void unmap() const override;
        void mapRange(uint32_t offset, uint32_t size) override;
        void flush() override;
        void flushRange(uint32_t offset, uint32_t size) override;

        VkBuffer vkBuffer() const { return m_buffer; }
        VkDeviceMemory vkDeviceMemory() const { return m_memory; }
        VkDeviceAddress deviceAddress() const;

    private:
        VkDevice m_device;
        VkBuffer m_buffer;
        VkDeviceMemory m_memory;
        // Persistently mapped pointer. vkMapMemory is called once at
        // construction and held for the buffer's lifetime; mapForWriting
        // and mapForReading just return this. The previous design called
        // vkMapMemory on every map and required a matching vkUnmapMemory;
        // any path that map'd twice without unmap'ing in between would
        // trip "Memory is already mapped" on the second call. With
        // HOST_COHERENT memory (which is what we allocate) holding the
        // map for the buffer's lifetime is zero-cost on UMA architectures
        // and avoids the brittle map/unmap discipline entirely.
        void *m_mapped = nullptr;
    };
} // namespace tracey