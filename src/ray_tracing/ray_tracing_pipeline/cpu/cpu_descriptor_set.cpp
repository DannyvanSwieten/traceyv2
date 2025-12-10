#include "cpu_descriptor_set.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
namespace tracey
{
    CpuDescriptorSet::CpuDescriptorSet(const RayTracingPipelineLayout &layout) : m_descriptors(layout.bindings().size())
    {
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
        m_descriptors[binding] = tlas;
    }
}