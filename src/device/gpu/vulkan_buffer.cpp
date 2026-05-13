#include "vulkan_buffer.hpp"
#include "vulkan_compute_device.hpp"
#include <stdexcept>
#include <array>
namespace tracey
{
    VulkanBuffer::VulkanBuffer(VulkanComputeDevice &device, uint32_t size, BufferUsage usageFlags) : m_device(device.vkDevice())
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;

        // Build Vulkan usage flags from tracey BufferUsage flags
        VkBufferUsageFlags vkUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if ((static_cast<uint32_t>(usageFlags) & static_cast<uint32_t>(BufferUsage::StorageBuffer)) != 0)
            vkUsage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if ((static_cast<uint32_t>(usageFlags) & static_cast<uint32_t>(BufferUsage::UniformBuffer)) != 0)
            vkUsage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if ((static_cast<uint32_t>(usageFlags) & static_cast<uint32_t>(BufferUsage::VertexBuffer)) != 0)
            vkUsage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if ((static_cast<uint32_t>(usageFlags) & static_cast<uint32_t>(BufferUsage::IndexBuffer)) != 0)
            vkUsage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        // If no specific usage was set, default to storage buffer for backwards compatibility
        if (vkUsage == (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
            vkUsage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        bufferInfo.usage = vkUsage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.queueFamilyIndexCount = 1;
        std::array<uint32_t, 1> queueFamilyIndices = {device.queueFamilyIndex()};
        bufferInfo.pQueueFamilyIndices = queueFamilyIndices.data();
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_buffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan buffer");
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(m_device, m_buffer, &memRequirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device.findMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_memory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate Vulkan buffer memory");
        }

        if (vkBindBufferMemory(m_device, m_buffer, m_memory, 0) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to bind Vulkan buffer memory");
        }

        // Persistently map for the buffer's lifetime. HOST_COHERENT
        // memory + a single vkMapMemory call means every subsequent
        // mapForWriting / mapForReading just returns the cached
        // pointer — no Vulkan API call, no risk of "already mapped"
        // / "not mapped" mismatches between map and unmap pairs.
        // Memory is released by the destructor's vkUnmapMemory.
        if (vkMapMemory(m_device, m_memory, 0, VK_WHOLE_SIZE, 0, &m_mapped) != VK_SUCCESS)
        {
            vkFreeMemory(m_device, m_memory, nullptr);
            vkDestroyBuffer(m_device, m_buffer, nullptr);
            throw std::runtime_error("Failed to persistently map Vulkan buffer memory");
        }
    }

    VulkanBuffer::~VulkanBuffer()
    {
        if (m_buffer != VK_NULL_HANDLE)
        {
            if (m_mapped)
            {
                vkUnmapMemory(m_device, m_memory);
                m_mapped = nullptr;
            }
            vkDestroyBuffer(m_device, m_buffer, nullptr);
            vkFreeMemory(m_device, m_memory, nullptr);
        }
    }

    void *VulkanBuffer::mapForWriting()
    {
        // Persistent map — see ctor. Callers expecting a fresh map
        // every call now get the same stable pointer; this is what
        // they wanted anyway for HOST_COHERENT memory.
        return m_mapped;
    }

    const void *VulkanBuffer::mapForReading() const
    {
        return m_mapped;
    }

    void VulkanBuffer::unmap() const
    {
        // No-op. Persistent mapping means the buffer stays mapped
        // until destruction. Keeping the method exists so callers
        // don't have to special-case between Cpu / Vulkan buffers.
    }

    void VulkanBuffer::mapRange(uint32_t offset, uint32_t size)
    {
        (void)offset;
        (void)size;
    }
    void VulkanBuffer::flush()
    {
        // HOST_COHERENT memory is auto-flushed on submit, so the
        // vkFlushMappedMemoryRanges call is a no-op in practice.
        // Keep the call shape for the rare case the allocator backs
        // a buffer with non-coherent memory in the future — and
        // explicitly do NOT unmap (the prior implementation did,
        // which surfaced as "Memory is already mapped" the next
        // time a caller mapped this buffer).
        VkMappedMemoryRange mappedRange{};
        mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mappedRange.memory = m_memory;
        mappedRange.offset = 0;
        mappedRange.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(m_device, 1, &mappedRange);
    }
    void VulkanBuffer::flushRange(uint32_t offset, uint32_t size)
    {
        (void)offset;
        (void)size;
        // No-op: persistent-mapped HOST_COHERENT memory needs neither
        // an explicit range flush nor an unmap.
    }
    VkDeviceAddress VulkanBuffer::deviceAddress() const
    {
        VkBufferDeviceAddressInfo bufferDeviceAI{};
        bufferDeviceAI.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        bufferDeviceAI.buffer = m_buffer;
        return vkGetBufferDeviceAddress(m_device, &bufferDeviceAI);
    }
}