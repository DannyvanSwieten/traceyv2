#pragma once

namespace tracey
{
    class RayTracingPipelineLayout;
    class CpuShaderBindingTable;

    void compileCpuRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt);
}