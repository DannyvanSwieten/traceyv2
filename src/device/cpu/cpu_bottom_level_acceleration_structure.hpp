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
        CpuBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount);

    private:
        std::optional<Blas> m_blas;
    };
} // namespace tracey