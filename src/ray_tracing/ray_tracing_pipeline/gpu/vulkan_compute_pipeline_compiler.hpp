#pragma once

namespace tracey
{
    class RayTracingPipelineLayout;
    class CpuShaderBindingTable;

    void compileVulkanComputeRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt);
}