#pragma once
#include <memory>
#include "../top_level_acceleration_structure.hpp"
#include "../../core/tlas.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class VulkanBuffer;
    class BottomLevelAccelerationStructure;
    class VulkanComputeTopLevelAccelerationStructure : public TopLevelAccelerationStructure
    {
    public:
        VulkanComputeTopLevelAccelerationStructure(VulkanComputeDevice &device, std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances);
        VulkanBuffer *instancesBuffer() const { return m_instancesBuffer.get(); }

    private:
        VulkanComputeDevice &m_device;
        std::unique_ptr<VulkanBuffer> m_instancesBuffer;
    };
}