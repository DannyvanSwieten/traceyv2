#pragma once

#include "../ray_tracing_pipeline.hpp"
#include <volk.h>

namespace tracey
{
    class VulkanComputeDevice;
    class CpuShaderBindingTable;
    class RayTracingPipelineLayout;
    class VulkanComputeRaytracingPipeline : public RayTracingPipeline
    {
    public:
        VulkanComputeRaytracingPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt);
        ~VulkanComputeRaytracingPipeline() override;

    private:
        VulkanComputeDevice &m_device;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    };
}