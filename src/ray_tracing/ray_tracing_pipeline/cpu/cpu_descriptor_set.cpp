#include "cpu_descriptor_set.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include <cassert>
namespace tracey
{
    CpuDescriptorSet::CpuDescriptorSet(const RayTracingPipelineLayoutDescriptor &layout) : m_descriptors(layout.bindings().size())
    {
        size_t index = 0;
        for (const auto &binding : layout.bindings())
        {
            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                m_descriptors[index] = static_cast<Image2D *>(nullptr);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::StorageBuffer:
                m_descriptors[index] = static_cast<Buffer *>(nullptr);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
                m_descriptors[index] = DispatchedTlas{nullptr, nullptr};
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::RayPayload:
                m_descriptors[index] = static_cast<void *>(nullptr);
                break;
            default:
                assert(false && "Unknown descriptor type");
                m_descriptors[index] = std::monostate{};
                break;
            }

            m_bindingIndices[binding.name] = index;
            ++index;
        }
    }

    void CpuDescriptorSet::setImage2D(const std::string_view binding, Image2D *image)
    {
        const auto index = m_bindingIndices.find(binding);
        assert(index != m_bindingIndices.end() && "Binding name not found in layout");
        const auto bindingIndex = index->second;
        m_descriptors[bindingIndex] = image;
    }
    void CpuDescriptorSet::setBuffer(const std::string_view binding, Buffer *buffer)
    {
        const auto index = m_bindingIndices.find(binding);
        assert(index != m_bindingIndices.end() && "Binding name not found in layout");
        const auto bindingIndex = index->second;
        m_descriptors[bindingIndex] = buffer;
    }
    void CpuDescriptorSet::setAccelerationStructure(const std::string_view binding, const TopLevelAccelerationStructure *tlas)
    {
        DispatchedTlas dispatched;
        dispatched.tlasInterface = tlas;
        const auto index = m_bindingIndices.find(binding);
        assert(index != m_bindingIndices.end() && "Binding name not found in layout");
        const auto bindingIndex = index->second;
        m_descriptors[bindingIndex] = dispatched;
    }
}