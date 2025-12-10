#include "cpu_ray_tracing_command_buffer.hpp"
#include "../../ray_tracing_pipeline/cpu/cpu_ray_tracing_pipeline.hpp"
#include <cassert>
namespace tracey
{
    CpuRayTracingCommandBuffer::CpuRayTracingCommandBuffer()
    {
    }
    void CpuRayTracingCommandBuffer::begin()
    {
    }

    void CpuRayTracingCommandBuffer::end()
    {
    }

    void CpuRayTracingCommandBuffer::setPipeline(const RayTracingPipeline *pipeline)
    {
        m_pipeline = dynamic_cast<const CpuRayTracingPipeline *>(pipeline);
    }
    void CpuRayTracingCommandBuffer::setDescriptorSet(const DescriptorSet *set)
    {
        m_descriptorSet = set;
    }
    void CpuRayTracingCommandBuffer::traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height)
    {
        if (!m_pipeline)
        {
            assert(false && "Pipeline not set for CpuRayTracingCommandBuffer");
            return;
        }

        // CPU-based ray tracing execution logic would go here
    }
}