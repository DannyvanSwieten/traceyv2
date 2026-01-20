#pragma once

#include "../bottom_level_acceleration_structure.hpp"
#include "../../core/blas.hpp"
#include <cstdint>
#include <optional>

namespace tracey
{
    class Buffer;
    class CpuBottomLevelAccelerationStructure : public BottomLevelAccelerationStructure
    {
    public:
        CpuBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount, const BVHConfig &bvhConfig = {});
        const Blas &blas() const { return m_blas.value(); }
        size_t nodeCount() const override { return m_blas ? m_blas->nodeCount() : 0; }

    private:
        std::optional<Blas> m_blas;
    };
} // namespace tracey