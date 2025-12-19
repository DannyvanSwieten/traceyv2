#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <optional>
#include "../bottom_level_acceleration_structure.hpp"
#include "../../core/blas.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class Buffer;

    class VulkanComputeBottomLevelAccelerationStructure : public BottomLevelAccelerationStructure
    {
    public:
        VulkanComputeBottomLevelAccelerationStructure(VulkanComputeDevice &device, const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount);
        ~VulkanComputeBottomLevelAccelerationStructure() override;

    private:
        VulkanComputeDevice &m_device;
        std::optional<Blas> m_blas;
        std::unique_ptr<Buffer> m_blas_buffer;
        VkDeviceAddress m_blas_device_address;

        VkDeviceMemory m_memory;
    };
}