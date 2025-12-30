#include "vulkan_compute_top_level_acceleration_structure.hpp"
#include "vulkan_compute_bottom_level_accelerations_structure.hpp"
#include "vulkan_buffer.hpp"
#include "vulkan_compute_device.hpp"
#include <cstdint>
#include <cstring>

namespace tracey
{
    // GPU BVH node layout (CPU-identical semantics, scalarized for std430)
    // Matches CPU BLAS node encoding:
    //  - firstChildOrPrim: inner = left child offset (right is implicit left+1), leaf = first index into primIndices()
    //  - primCountAndType: inner = 0, leaf = primitive count in low 24 bits (type in high 8 bits)
    struct GpuBvhNode
    {
        float minx, miny, minz;
        uint32_t firstChildOrPrim;
        float maxx, maxy, maxz;
        uint32_t primCountAndType;
    };

    VulkanComputeTopLevelAccelerationStructure::VulkanComputeTopLevelAccelerationStructure(VulkanComputeDevice &device, std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances) : m_device(device)
    {
        size_t nodeCount = 0;
        size_t triangleCount = 0;
        // Count the total number of bvh nodes
        for (const auto &blas : blases)
        {
            const auto vulkanBlas = static_cast<const VulkanComputeBottomLevelAccelerationStructure *>(blas);
            nodeCount += vulkanBlas->nodeCount();
            triangleCount += vulkanBlas->triangleCount();
        }

        m_blasBuffer = std::make_unique<VulkanBuffer>(m_device, sizeof(GpuBvhNode) * static_cast<uint32_t>(nodeCount), BufferUsage::StorageBuffer);
        m_triangleInfoBuffer = std::make_unique<VulkanBuffer>(m_device, sizeof(Blas::TriangleData) * triangleCount, BufferUsage::StorageBuffer);
        m_instancesBuffer = std::make_unique<VulkanBuffer>(m_device, sizeof(Tlas::Instance) * static_cast<uint32_t>(instances.size()) + sizeof(uint32_t) * 4, BufferUsage::StorageBuffer);
        m_blasInfoBuffer = std::make_unique<VulkanBuffer>(m_device, sizeof(BlasInfo) * static_cast<uint32_t>(blases.size()), BufferUsage::StorageBuffer);
        m_primitiveIndicesBuffer = std::make_unique<VulkanBuffer>(m_device, sizeof(uint32_t) * triangleCount, BufferUsage::StorageBuffer);

        // Copy all blas nodes into a single buffer
        BVHNode *nodeData = static_cast<BVHNode *>(m_blasBuffer->mapForWriting());
        Blas::TriangleData *triangleData = static_cast<Blas::TriangleData *>(m_triangleInfoBuffer->mapForWriting());
        uint32_t *instanceCountData = static_cast<uint32_t *>(m_instancesBuffer->mapForWriting());
        *instanceCountData = static_cast<uint32_t>(instances.size());
        instanceCountData += 4;
        Tlas::Instance *instanceData = reinterpret_cast<Tlas::Instance *>(instanceCountData);
        std::memcpy(instanceData, instances.data(), sizeof(Tlas::Instance) * instances.size());
        BlasInfo *blasInfoData = static_cast<BlasInfo *>(m_blasInfoBuffer->mapForWriting());
        uint32_t *primitiveIndexData = static_cast<uint32_t *>(m_primitiveIndicesBuffer->mapForWriting());

        size_t nodeOffset = 0;
        size_t triangleOffset = 0;
        size_t blasIndex = 0;
        for (const auto &blas : blases)
        {
            const auto vulkanBlas = static_cast<const VulkanComputeBottomLevelAccelerationStructure *>(blas);
            const auto blasNodeCount = vulkanBlas->nodeCount();
            const auto blasTriangleCount = vulkanBlas->triangleCount();
            std::memcpy(&triangleData[triangleOffset], vulkanBlas->triangleData().data(), sizeof(Blas::TriangleData) * blasTriangleCount);
            std::memcpy(&primitiveIndexData[triangleOffset], vulkanBlas->primIndices().data(), sizeof(uint32_t) * blasTriangleCount);
            blasInfoData[blasIndex].rootNodeIndex = static_cast<uint>(nodeOffset);
            blasInfoData[blasIndex].triangleOffset = static_cast<uint>(triangleOffset);

            // Upload BVH nodes for this BLAS into the global node buffer (CPU-identical semantics).
            const auto &srcNodes = vulkanBlas->nodes();
            std::memcpy(&nodeData[nodeOffset], srcNodes.data(), sizeof(BVHNode) * blasNodeCount);

            nodeOffset += blasNodeCount;
            triangleOffset += blasTriangleCount;
            blasIndex++;
        }

        m_blasBuffer->flush();
        m_triangleInfoBuffer->flush();
        m_instancesBuffer->flush();
        m_blasInfoBuffer->flush();
        m_primitiveIndicesBuffer->flush();
    }
} // namespace tracey