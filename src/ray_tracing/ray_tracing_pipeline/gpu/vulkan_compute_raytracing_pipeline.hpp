#pragma once

#include "../ray_tracing_pipeline.hpp"
#include <volk.h>

namespace tracey
{
    class VulkanComputeDevice;
    class VulkanComputeRaytracingPipeline : public RayTracingPipeline
    {
    public:
        VulkanComputeRaytracingPipeline(VulkanComputeDevice &device);
        ~VulkanComputeRaytracingPipeline() override;

    private:
        VulkanComputeDevice &m_device;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    };
}