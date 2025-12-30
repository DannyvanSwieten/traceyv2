#include "vulkan_buffer.hpp"
#include "vulkan_compute_device.hpp"
#include <stdexcept>
namespace tracey
{
    VulkanBuffer::VulkanBuffer(VulkanComputeDevice &device, uint32_t size, BufferUsage usageFlags) : m_device(device.vkDevice())
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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
    }

    VulkanBuffer::~VulkanBuffer()
    {
        if (m_buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, m_buffer, nullptr);
            vkFreeMemory(m_device, m_memory, nullptr);
        }
    }

    void *VulkanBuffer::mapForWriting()
    {
        VkMappedMemoryRange mappedRange{};
        mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mappedRange.memory = m_memory;
        mappedRange.offset = 0;
        mappedRange.size = VK_WHOLE_SIZE;
        void *data;
        vkMapMemory(m_device, m_memory, 0, VK_WHOLE_SIZE, 0, &data);
        return data;
    }

    const void *VulkanBuffer::mapForReading() const
    {
        VkMappedMemoryRange mappedRange{};
        mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mappedRange.memory = m_memory;
        mappedRange.offset = 0;
        mappedRange.size = VK_WHOLE_SIZE;
        const void *data;
        vkMapMemory(m_device, m_memory, 0, VK_WHOLE_SIZE, 0, (void **)&data);
        return data;
    }

    void VulkanBuffer::mapRange(uint32_t offset, uint32_t size)
    {
        (void)offset;
        (void)size;
    }
    void VulkanBuffer::flush()
    {
        VkMappedMemoryRange mappedRange{};
        mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mappedRange.memory = m_memory;
        mappedRange.offset = 0;
        mappedRange.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(m_device, 1, &mappedRange);
        vkUnmapMemory(m_device, m_memory);
    }
    void VulkanBuffer::flushRange(uint32_t offset, uint32_t size)
    {
        (void)offset;
        (void)size;
        vkUnmapMemory(m_device, m_memory);
    }
    VkDeviceAddress VulkanBuffer::deviceAddress() const
    {
        VkBufferDeviceAddressInfo bufferDeviceAI{};
        bufferDeviceAI.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        bufferDeviceAI.buffer = m_buffer;
        return vkGetBufferDeviceAddress(m_device, &bufferDeviceAI);
    }
}