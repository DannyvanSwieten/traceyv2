#pragma once
#include "../ray_tracing_pipeline.hpp"
#include "cpu_pipeline_compiler.hpp"
namespace tracey
{
    class CpuShaderBindingTable;
    class RayTracingPipelineLayout;
    class CpuShaderModule;
    class CpuRayTracingPipeline : public RayTracingPipeline
    {
    public:
        CpuRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt);
        const RayTracingPipelineLayout &layout() const;
        const CpuShaderBindingTable &sbt() const;
        Sbt &compiledSbt() { return m_compiledSbt; }

    private:
        Sbt compileShaders();
        CompiledShader compileShader(const CpuShaderModule &module);

    private:
        const RayTracingPipelineLayout &m_layout;
        const CpuShaderBindingTable &m_sbt;
        Sbt m_compiledSbt;
    };
} // namespace tracey