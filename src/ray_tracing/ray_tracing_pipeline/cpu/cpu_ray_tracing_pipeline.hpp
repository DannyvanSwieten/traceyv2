#pragma once
#include "../ray_tracing_pipeline.hpp"
#include "cpu_pipeline_compiler.hpp"
namespace tracey
{
    class CpuShaderBindingTable;
    class RayTracingPipelineLayoutDescriptor;
    class CpuShaderModule;
    class CpuRayTracingPipeline : public RayTracingPipeline
    {
    public:
        CpuRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);

        void allocateDescriptorSets(std::span<DescriptorSet *> sets) override;

        const RayTracingPipelineLayoutDescriptor &layout() const;
        const CpuShaderBindingTable &sbt() const;
        Sbt &compiledSbt() { return m_compiledSbt; }

    private:
        Sbt compileShaders();
        CompiledShader compileShader(const CpuShaderModule &module);

    private:
        const RayTracingPipelineLayoutDescriptor &m_layout;
        const CpuShaderBindingTable &m_sbt;
        Sbt m_compiledSbt;
    };
} // namespace tracey