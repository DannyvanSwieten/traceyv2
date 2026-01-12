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
        ~VulkanWaveFrontPipeline();
        void allocateDescriptorSets(std::span<DescriptorSet *> sets) override;

        VkPipeline rayGenPipeline() const { return m_rayGenPipelineInfo.pipeline; }
        VkPipeline intersectPipeline() const { return m_intersectPipelineInfo.pipeline; }
        VkPipeline hitPipeline(size_t index) const { return m_hitPipelines[index].pipeline; }
        VkPipeline missPipeline(size_t index) const { return m_missPipelines[index].pipeline; }
        VkPipelineLayout pipelineLayout() const { return m_rayGenPipelineInfo.pipelineLayout; }

        struct PipelineInfo
        {
            VkShaderModule shaderModule = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        };

    private:
        size_t bindingStartOffset(ShaderStage stage) const;

    private:
        VulkanComputeDevice &m_device;
        const RayTracingPipelineLayoutDescriptor &m_layout;
        PipelineInfo m_rayGenPipelineInfo;
        PipelineInfo m_intersectPipelineInfo;
        std::vector<PipelineInfo> m_hitPipelines;
        std::vector<PipelineInfo> m_missPipelines;
    };

}