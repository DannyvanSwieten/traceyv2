#pragma once
#include <vector>

namespace tracey
{
    class RayTracingPipelineLayoutDescriptor;
    class CpuShaderBindingTable;

    std::vector<uint32_t> compileVulkanComputeRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);

    std::vector<uint32_t> compileRayGenShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);
}