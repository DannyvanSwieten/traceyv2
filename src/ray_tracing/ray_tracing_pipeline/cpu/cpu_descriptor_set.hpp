#pragma once

#include "../descriptor_set.hpp"
#include <vector>
#include <variant>

namespace tracey
{
    class RayTracingPipelineLayout;
    class CpuRayTracingPipeline;
    struct DispatchedTlas
    {
        const TopLevelAccelerationStructure *tlasInterface = nullptr;
        CpuRayTracingPipeline *pipeline = nullptr;
    };
    class CpuDescriptorSet : public DescriptorSet
    {
    public:
        CpuDescriptorSet(const RayTracingPipelineLayout &layout);

        void setImage2D(uint32_t binding, Image2D *image) override;
        void setBuffer(uint32_t binding, Buffer *buffer) override;
        void setAccelerationStructure(uint32_t binding, const TopLevelAccelerationStructure *tlas) override;

        template <typename Visitor>
        auto visit(Visitor &&visitor, uint32_t binding) const
        {
            return std::visit(visitor, m_descriptors[binding]);
        }

        template <typename Visitor>
        auto visitMut(Visitor &&visitor, uint32_t binding)
        {
            return std::visit(visitor, m_descriptors[binding]);
        }

    private:
        using DescriptorValue = std::variant<std::monostate, Image2D *, Buffer *, DispatchedTlas>;
        std::vector<DescriptorValue> m_descriptors;
    };
} // namespace tracey