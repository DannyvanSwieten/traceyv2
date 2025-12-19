#pragma once
#include <memory>
#include <volk.h>
#include <optional>
#include "../bottom_level_acceleration_structure.hpp"
#include "../../core/blas.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class Buffer;
    class VulkanBuffer;

    class VulkanComputeBottomLevelAccelerationStructure : public BottomLevelAccelerationStructure
    {
    public:
        VulkanComputeBottomLevelAccelerationStructure(VulkanComputeDevice &device, const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount);

    private:
        VulkanComputeDevice &m_device;
        std::optional<Blas> m_blas;
        std::unique_ptr<VulkanBuffer> m_blas_buffer;
        VkDeviceAddress m_blas_device_address;
    };
}