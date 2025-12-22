#include "vulkan_compute_raytracing_pipeline.hpp"
#include "../../../device/gpu/vulkan_compute_device.hpp"
#include "vulkan_compute_pipeline_compiler.hpp"
// #include "../ray_tracing_pipeline_layout.hpp"
namespace tracey
{
    VulkanComputeRaytracingPipeline::VulkanComputeRaytracingPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt)
        : m_device(device)
    {
        compileVulkanComputeRayTracingPipeline(layout, sbt);
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