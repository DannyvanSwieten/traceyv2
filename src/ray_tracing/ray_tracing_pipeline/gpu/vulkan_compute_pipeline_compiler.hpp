#pragma once
#include <vector>

namespace tracey
{
    class RayTracingPipelineLayoutDescriptor;
    class CpuShaderBindingTable;

    struct WaveFrontPipelineCompileResult
    {
        std::vector<uint32_t> rayGenShaderSpirV;
        std::vector<uint32_t> intersectShaderSpirV;
        std::vector<std::vector<uint32_t>> hitShadersSpirV;
        std::vector<std::vector<uint32_t>> missShadersSpirV;
        std::vector<uint32_t> resolveShaderSpirV;
    };

    std::vector<uint32_t> compileVulkanComputeRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);

    WaveFrontPipelineCompileResult compileVulkanWaveFrontRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);

    std::vector<uint32_t> compileRayGenShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);
    std::vector<uint32_t> compileHitShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt, size_t hitShaderIndex);
    std::vector<uint32_t> compileMissShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt, size_t missShaderIndex);
    std::vector<uint32_t> compileResolveShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);
}