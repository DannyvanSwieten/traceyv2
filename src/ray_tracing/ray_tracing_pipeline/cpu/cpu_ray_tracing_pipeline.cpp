#include "cpu_ray_tracing_pipeline.hpp"
#include "cpu_shader_binding_table.hpp"
namespace tracey
{
    CpuRayTracingPipeline::CpuRayTracingPipeline(const CpuShaderBindingTable &sbt, const RayTracingPipelineLayout &layout) : m_sbt(sbt), m_layout(layout)
    {
    }
    const RayTracingPipelineLayout &CpuRayTracingPipeline::layout() const
    {
        return m_layout;
    }
    const CpuShaderBindingTable &CpuRayTracingPipeline::sbt() const
    {
        return m_sbt;
    }
}