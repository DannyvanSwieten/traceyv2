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

        size_t nodeCount() const { return m_blas->nodeCount(); }
        std::span<const BVHNode> nodes() const { return std::span<const BVHNode>(m_blas->nodes().data(), m_blas->nodeCount()); }
        size_t triangleCount() const;
        const std::span<const Blas::TriangleData> triangleData() const { return std::span<const Blas::TriangleData>(m_blas->triangleData().data(), m_blas->triangleData().size()); }
        const std::span<const uint32_t> primIndices() const { return std::span<const uint32_t>(m_blas->primIndices().data(), m_blas->primIndices().size()); }

    private:
        VulkanComputeDevice &m_device;
        std::optional<Blas> m_blas;
    };
}