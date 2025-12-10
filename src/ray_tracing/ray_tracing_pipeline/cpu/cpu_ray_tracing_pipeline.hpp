#pragma once
#include "../ray_tracing_pipeline.hpp"
namespace tracey
{
    class CpuShaderBindingTable;
    class RayTracingPipelineLayout;
    class CpuRayTracingPipeline : public RayTracingPipeline
    {
    public:
        CpuRayTracingPipeline(const CpuShaderBindingTable &sbt, const RayTracingPipelineLayout &layout);
        const RayTracingPipelineLayout &layout() const;
        const CpuShaderBindingTable &sbt() const;

    private:
        const CpuShaderBindingTable &m_sbt;
        const RayTracingPipelineLayout &m_layout;
    };
} // namespace tracey