#pragma once
#include <volk.h>
#include "../../ray_tracing_pipeline.hpp"
namespace tracey
{
    class RayTracingPipelineLayoutDescriptor;
    class CpuShaderBindingTable;
    class VulkanComputeDevice;

    class VulkanWaveFrontPipeline : public RayTracingPipeline
    {
    public:
        VulkanWaveFrontPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt);
        void allocateDescriptorSets(std::span<DescriptorSet *> sets) override;

        struct PipelineInfo
        {
            VkShaderModule shaderModule = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        };

    private:
        VulkanComputeDevice &m_device;
        PipelineInfo m_rayGenPipelineInfo;
        PipelineInfo m_intersectPipelineInfo;
        PipelineInfo m_hitPipelineInfo;
        std::vector<PipelineInfo> m_hitPipelines;
    };

}