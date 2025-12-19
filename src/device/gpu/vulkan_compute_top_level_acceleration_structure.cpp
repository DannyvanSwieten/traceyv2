#include "vulkan_compute_top_level_acceleration_structure.hpp"
#include "vulkan_compute_bottom_level_accelerations_structure.hpp"
#include "vulkan_buffer.hpp"
#include "vulkan_compute_device.hpp"

namespace tracey
{
    VulkanComputeTopLevelAccelerationStructure::VulkanComputeTopLevelAccelerationStructure(VulkanComputeDevice &device, std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances) : m_device(device)
    {
        m_instancesBuffer = std::make_unique<VulkanBuffer>(m_device, sizeof(Tlas::Instance) * static_cast<uint32_t>(instances.size()), BufferUsage::StorageBuffer);
        Tlas::Instance *data = static_cast<Tlas::Instance *>(m_instancesBuffer->mapForWriting());
        for (auto i = 0u; i < blases.size(); ++i)
        {
            const auto vulkanBlas = static_cast<const VulkanComputeBottomLevelAccelerationStructure *>(blases[i]);
            data[i].blasAddress = vulkanBlas->address();
        }
        std::memcpy(data, instances.data(), sizeof(Tlas::Instance) * instances.size());
        m_instancesBuffer->flush();
    }
}