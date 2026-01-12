#pragma once

#include "../descriptor_set.hpp"
#include <vector>
#include <variant>
#include <map>

namespace tracey
{
    class RayTracingPipelineLayoutDescriptor;
    class CpuRayTracingPipeline;
    struct DispatchedTlas
    {
        const TopLevelAccelerationStructure *tlasInterface = nullptr;
        CpuRayTracingPipeline *pipeline = nullptr;
    };
    class CpuDescriptorSet : public DescriptorSet
    {
    public:
        CpuDescriptorSet(const RayTracingPipelineLayoutDescriptor &layout);

        void setImage2D(const std::string_view binding, Image2D *image) override;
        void setBuffer(const std::string_view binding, Buffer *buffer) override;
        void setAccelerationStructure(const std::string_view binding, const TopLevelAccelerationStructure *tlas) override;
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
        using DescriptorValue = std::variant<std::monostate, Image2D *, Buffer *, DispatchedTlas, void *>;
        std::vector<DescriptorValue> m_descriptors;
        std::map<std::string_view, uint32_t> m_bindingIndices;
    };
} // namespace tracey