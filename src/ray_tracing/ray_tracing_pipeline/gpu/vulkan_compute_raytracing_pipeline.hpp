#pragma once

#include "../ray_tracing_pipeline.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include <volk.h>

namespace tracey
{
    class VulkanComputeDevice;
    class CpuShaderBindingTable;
    class VulkanComputeRaytracingPipeline : public RayTracingPipeline
    {
    public:
        VulkanComputeRaytracingPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);
        ~VulkanComputeRaytracingPipeline() override;

        void allocateDescriptorSets(std::span<DescriptorSet *> sets) override;

        VkPipeline vkPipeline() const { return m_pipeline; }
        VkPipelineLayout vkPipelineLayout() const { return m_pipelineLayout; }

    private:
        VulkanComputeDevice &m_device;
        RayTracingPipelineLayoutDescriptor m_layout;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayoutCreateInfo m_descriptorSetLayoutInfo{};
        VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
        VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    };
}