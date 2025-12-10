#include "cpu_bottom_level_acceleration_structure.hpp"
#include "cpu_buffer.hpp"
namespace tracey
{
    CpuBottomLevelAccelerationStructure::CpuBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount)
    {
        const auto posData = static_cast<const float *>(positions->mapForReading());
        const auto indexData = static_cast<const uint32_t *>(indices->mapForReading());
        const auto stride = positionStride / sizeof(float);
        const std::span<const float> positionsSpan(posData, positionCount * stride);
        const std::span<const uint32_t> indicesSpan(indexData, indexCount);

        m_blas.emplace(positionsSpan, stride, indicesSpan);
    }
} // namespace tracey