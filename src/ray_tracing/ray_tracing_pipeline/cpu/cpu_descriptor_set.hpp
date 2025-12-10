#pragma once

#include "../descriptor_set.hpp"
#include <vector>
#include <variant>

namespace tracey
{
    class RayTracingPipelineLayout;
    class CpuDescriptorSet : public DescriptorSet
    {
    public:
        CpuDescriptorSet(const RayTracingPipelineLayout &layout);

        void setImage2D(uint32_t binding, Image2D *image) override;
        void setBuffer(uint32_t binding, Buffer *buffer) override;
        void setAccelerationStructure(uint32_t binding, const TopLevelAccelerationStructure *tlas) override;

    private:
        using DescriptorValue = std::variant<std::monostate, Image2D *, Buffer *, const TopLevelAccelerationStructure *>;
        std::vector<DescriptorValue> m_descriptors;
    };
} // namespace tracey