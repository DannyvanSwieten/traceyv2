#pragma once
#include "../ray_tracing_pipeline.hpp"
namespace tracey
{
    class RayTracingPipelineLayoutDescriptor;
    class CpuShaderBindingTable;
    class VulkanComputeDevice;

    class VulkanWaveFrontPipeline : public RayTracingPipeline
    {
    public:
        VulkanWaveFrontPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);
        ~VulkanWaveFrontPipeline() override;
    };

}