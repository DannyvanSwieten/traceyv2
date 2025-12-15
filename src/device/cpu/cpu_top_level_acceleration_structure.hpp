#pragma once

#include "../top_level_acceleration_structure.hpp"
#include "../bottom_level_acceleration_structure.hpp"
#include "../../core/tlas.hpp"
#include "cpu_bottom_level_acceleration_structure.hpp"

namespace tracey
{
    class CpuTopLevelAccelerationStructure : public TopLevelAccelerationStructure
    {
    public:
        CpuTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances) : m_blases(blases)
        {
            for (const auto &blas : blases)
            {
                const auto cpuBlas = dynamic_cast<const CpuBottomLevelAccelerationStructure *>(blas);
                if (cpuBlas)
                {
                    blasPtrs.push_back(&cpuBlas->blas());
                }
            }
            m_tlas.emplace(std::span<const Blas *>(blasPtrs.data(), blasPtrs.size()), instances);
        }

        const Tlas &tlas() const { return m_tlas.value(); }

    private:
        std::vector<const Blas *> blasPtrs;
        std::span<const BottomLevelAccelerationStructure *> m_blases;
        std::optional<Tlas> m_tlas;
    };
} // namespace tracey