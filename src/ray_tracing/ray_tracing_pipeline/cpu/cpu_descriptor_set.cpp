#include "cpu_descriptor_set.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include <cassert>
namespace tracey
{
    CpuDescriptorSet::CpuDescriptorSet(const RayTracingPipelineLayout &layout) : m_descriptors(layout.bindings().size())
    {
        for (const auto &binding : layout.bindings())
        {
            switch (binding.type)
            {
            case RayTracingPipelineLayout::DescriptorType::Image2D:
                m_descriptors[binding.index] = static_cast<Image2D *>(nullptr);
                break;
            case RayTracingPipelineLayout::DescriptorType::Buffer:
                m_descriptors[binding.index] = static_cast<Buffer *>(nullptr);
                break;
            case RayTracingPipelineLayout::DescriptorType::AccelerationStructure:
                m_descriptors[binding.index] = DispatchedTlas{nullptr, nullptr};
                break;
            default:
                assert(false && "Unknown descriptor type");
                m_descriptors[binding.index] = std::monostate{};
                break;
            }
        }
    }

    void CpuDescriptorSet::setImage2D(uint32_t binding, Image2D *image)
    {
        m_descriptors[binding] = image;
    }
    void CpuDescriptorSet::setBuffer(uint32_t binding, Buffer *buffer)
    {
        m_descriptors[binding] = buffer;
    }
    void CpuDescriptorSet::setAccelerationStructure(uint32_t binding, const TopLevelAccelerationStructure *tlas)
    {
        DispatchedTlas dispatched;
        dispatched.tlasInterface = tlas;
        m_descriptors[binding] = dispatched;
    }
}