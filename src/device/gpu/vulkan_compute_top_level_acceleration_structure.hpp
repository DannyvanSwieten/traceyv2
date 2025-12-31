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
        VulkanBuffer *asBuffer() const { return m_blasBuffer.get(); }
        VulkanBuffer *triangleInfoBuffer() const { return m_triangleInfoBuffer.get(); }
        VulkanBuffer *blasInfoBuffer() const { return m_blasInfoBuffer.get(); }
        VulkanBuffer *primitiveIndicesBuffer() const { return m_primitiveIndicesBuffer.get(); }

        VulkanBuffer *instanceInverseTransformsBuffer() const { return m_instanceInverseTransformsBuffer.get(); }

    private:
        VulkanComputeDevice &m_device;
        std::unique_ptr<VulkanBuffer> m_instancesBuffer;
        std::unique_ptr<VulkanBuffer> m_blasBuffer;
        std::unique_ptr<VulkanBuffer> m_blasInfoBuffer;
        std::unique_ptr<VulkanBuffer> m_triangleInfoBuffer;
        std::unique_ptr<VulkanBuffer> m_primitiveIndicesBuffer;
        std::unique_ptr<VulkanBuffer> m_instanceInverseTransformsBuffer;

        struct BlasInfo
        {
            uint rootNodeIndex;
            uint triangleCount;
            uint triangleOffset;
            uint padding0;
        };
    };
}