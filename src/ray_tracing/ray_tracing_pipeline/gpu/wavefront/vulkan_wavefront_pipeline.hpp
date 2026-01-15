#pragma once
#include <volk.h>
#include "../../ray_tracing_pipeline.hpp"
namespace tracey
{
    enum class ShaderStage;
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
        VkPipeline resolvePipeline() const { return m_resolvePipelineInfo.pipeline; }
        VkPipelineLayout pipelineLayout() const { return m_rayGenPipelineInfo.pipelineLayout; }

        VkBuffer hitInfoBuffer() const { return m_hitInfoBuffer; }
        VkBuffer rayQueueBuffer() const { return m_rayQueueBuffer; }
        VkBuffer rayQueueBuffer2() const { return m_rayQueueBuffer2; }
        uint32_t maxRayCount() const { return m_maxRayCount; }

        struct PipelineInfo
        {
            VkShaderModule shaderModule = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        };

    private:
        size_t bindingStartOffset(ShaderStage stage) const;
        void allocateInternalBuffers(uint32_t maxRayCount);
        void bindInternalBuffers(VkDescriptorSet descriptorSet, bool swapQueues = false);

    private:
        VulkanComputeDevice &m_device;
        const RayTracingPipelineLayoutDescriptor &m_layout;
        PipelineInfo m_rayGenPipelineInfo;
        PipelineInfo m_intersectPipelineInfo;
        std::vector<PipelineInfo> m_hitPipelines;
        std::vector<PipelineInfo> m_missPipelines;
        PipelineInfo m_resolvePipelineInfo;

        // Wavefront internal buffers
        VkBuffer m_payloadBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_payloadMemory = VK_NULL_HANDLE;
        VkBuffer m_pathHeaderBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_pathHeaderMemory = VK_NULL_HANDLE;
        VkBuffer m_rayQueueBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_rayQueueMemory = VK_NULL_HANDLE;
        VkBuffer m_rayQueueBuffer2 = VK_NULL_HANDLE;
        VkDeviceMemory m_rayQueueMemory2 = VK_NULL_HANDLE;
        VkBuffer m_hitInfoBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_hitInfoMemory = VK_NULL_HANDLE;
        uint32_t m_maxRayCount = 0;
        size_t m_payloadSize = 0;
    };

}