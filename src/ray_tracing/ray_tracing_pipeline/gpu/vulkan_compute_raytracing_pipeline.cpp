#include "vulkan_compute_raytracing_pipeline.hpp"
#include "../../../device/gpu/vulkan_compute_device.hpp"
#include "vulkan_compute_pipeline_compiler.hpp"
#include "vulkan_compute_raytracing_descriptor_set.hpp"
#include <cassert>
#include <stdexcept>
namespace tracey
{
    VulkanComputeRaytracingPipeline::VulkanComputeRaytracingPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt) : m_device(device), m_layout(layout)
    {
        const auto spirvCode = compileVulkanComputeRayTracingPipeline(layout, sbt);
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        // Fill layoutInfo based on pipeline layout
        for (const auto &binding : layout.bindings())
        {
            VkDescriptorSetLayoutBinding vkBinding{};
            vkBinding.binding = binding.index;
            vkBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; // Adjust based on ShaderStage
            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                vkBinding.descriptorCount = 1;
                vkBinding.binding += 6;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Buffer:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                vkBinding.descriptorCount = 1;
                vkBinding.binding += 6;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
                assert(binding.index == 0 && "AccelerationStructure binding index must be 0");
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                vkBinding.descriptorCount = 1;
                vkBinding.binding = binding.index;
                bindings.push_back(vkBinding);
                vkBinding.binding = binding.index + 1;
                bindings.push_back(vkBinding);
                vkBinding.binding = binding.index + 2;
                bindings.push_back(vkBinding);
                vkBinding.binding = binding.index + 3;
                bindings.push_back(vkBinding);
                vkBinding.binding = binding.index + 4;
                bindings.push_back(vkBinding);
                vkBinding.binding = binding.index + 5;
                bindings.push_back(vkBinding);

                break;
            default:
                throw std::runtime_error("Unsupported descriptor type");
            }
        }
        layoutInfo.pBindings = bindings.data();
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());

        if (vkCreateDescriptorSetLayout(m_device.vkDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create descriptor set layout");
        }

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
        if (vkCreatePipelineLayout(m_device.vkDevice(), &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create pipeline layout");
        }

        VkShaderModuleCreateInfo shaderModuleInfo{};
        shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderModuleInfo.codeSize = spirvCode.size() * sizeof(uint32_t);
        shaderModuleInfo.pCode = spirvCode.data();

        if (vkCreateShaderModule(m_device.vkDevice(), &shaderModuleInfo, nullptr, &m_shaderModule) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create shader module");
        }

        m_shaderModule = m_shaderModule;
        // Create compute pipeline
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = m_shaderModule;
        pipelineInfo.stage.pName = "main"; // Entry point

        if (vkCreateComputePipelines(m_device.vkDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create compute pipeline");
        }
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
        if (m_shaderModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(m_device.vkDevice(), m_shaderModule, nullptr);
        }
    }
    void VulkanComputeRaytracingPipeline::allocateDescriptorSets(std::span<DescriptorSet *> sets)
    {
        for (auto &set : sets)
        {
            auto vkSet = new VulkanComputeRayTracingDescriptorSet(m_device, m_descriptorSetLayout);
            set = vkSet;
        }
    }
} // namespace tracey