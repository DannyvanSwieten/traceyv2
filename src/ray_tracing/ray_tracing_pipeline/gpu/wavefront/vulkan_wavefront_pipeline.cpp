#include "vulkan_wavefront_pipeline.hpp"
#include "../vulkan_compute_pipeline_compiler.hpp"
#include "../../../../device/gpu/vulkan_compute_device.hpp"

namespace tracey
{
    VulkanWaveFrontPipeline::VulkanWaveFrontPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt) : m_device(device)
    {
        const auto wavefrontCompilerResult = compileVulkanWaveFrontRayTracingPipeline(layout, sbt);
        PipelineInfo rayGenPipelineInfo{};
        // Create ray generation pipeline
        {
            VkShaderModuleCreateInfo shaderModuleInfo{};
            shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderModuleInfo.codeSize = wavefrontCompilerResult.rayGenShaderSpirV.size() * sizeof(uint32_t);
            shaderModuleInfo.pCode = wavefrontCompilerResult.rayGenShaderSpirV.data();
            if (vkCreateShaderModule(m_device.vkDevice(), &shaderModuleInfo, nullptr, &rayGenPipelineInfo.shaderModule) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create ray generation shader module");
            }

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
            pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        }
    }
    void VulkanWaveFrontPipeline::allocateDescriptorSets(std::span<DescriptorSet *> sets)
    {
    }
}
