#include "vulkan_compute_raytracing_pipeline.hpp"
#include "../../../device/gpu/vulkan_compute_device.hpp"
namespace tracey
{
    VulkanComputeRaytracingPipeline::VulkanComputeRaytracingPipeline(VulkanComputeDevice &device)
        : m_device(device)
    {
    }

    VulkanComputeRaytracingPipeline::~VulkanComputeRaytracingPipeline()
    {
        if (m_pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device.vkDevice(), m_pipeline, nullptr);
        }
        if (m_pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device.vkDevice(), m_pipelineLayout, nullptr);
        }
        if (m_descriptorSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device.vkDevice(), m_descriptorSetLayout, nullptr);
        }
    }
} // namespace tracey