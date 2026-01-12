#include "vulkan_wavefront_pipeline.hpp"
#include "../vulkan_compute_pipeline_compiler.hpp"
#include "../vulkan_compute_raytracing_descriptor_set.hpp"
#include "../../ray_tracing_pipeline_layout.hpp"
#include "../../../../device/gpu/vulkan_compute_device.hpp"

namespace tracey
{
    VulkanWaveFrontPipeline::VulkanWaveFrontPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt) : m_device(device), m_layout(layout)
    {
        const auto wavefrontCompilerResult = compileVulkanWaveFrontRayTracingPipeline(layout, sbt);

        // Create descriptor set layout (shared by all pipelines)
        std::vector<VkDescriptorSetLayoutBinding> bindings;

        // Reserve first 5 bindings (0-4) for acceleration structure like monolithic pipeline
        const size_t bindingStartOffset = 5;

        for (const auto &binding : layout.bindings())
        {
            const size_t bindingIndex = layout.indexForBinding(binding.name);
            VkDescriptorSetLayoutBinding vkBinding{};
            vkBinding.binding = bindingIndex;
            vkBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                vkBinding.descriptorCount = 1;
                vkBinding.binding += bindingStartOffset;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::StorageBuffer:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                vkBinding.descriptorCount = 1;
                vkBinding.binding += bindingStartOffset;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
                // TLAS requires 6 storage buffers (bindings 0-5)
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                vkBinding.descriptorCount = 1;
                for (size_t i = 0; i < 6; ++i)
                {
                    vkBinding.binding = bindingIndex + i;
                    bindings.push_back(vkBinding);
                }
                break;
            default:
                throw std::runtime_error("Unsupported descriptor type");
            }
        }

        VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
        descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorSetLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        descriptorSetLayoutInfo.pBindings = bindings.data();

        VkDescriptorSetLayout descriptorSetLayout;
        if (vkCreateDescriptorSetLayout(m_device.vkDevice(), &descriptorSetLayoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create descriptor set layout");
        }

        // Create pipeline layout (shared by all pipelines)
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(uint32_t) * 2; // width, height

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        VkPipelineLayout pipelineLayout;
        if (vkCreatePipelineLayout(m_device.vkDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create pipeline layout");
        }

        // Create ray generation pipeline
        {
            VkShaderModuleCreateInfo shaderModuleInfo{};
            shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderModuleInfo.codeSize = wavefrontCompilerResult.rayGenShaderSpirV.size() * sizeof(uint32_t);
            shaderModuleInfo.pCode = wavefrontCompilerResult.rayGenShaderSpirV.data();
            if (vkCreateShaderModule(m_device.vkDevice(), &shaderModuleInfo, nullptr, &m_rayGenPipelineInfo.shaderModule) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create ray generation shader module");
            }

            m_rayGenPipelineInfo.pipelineLayout = pipelineLayout;
            m_rayGenPipelineInfo.descriptorSetLayout = descriptorSetLayout;

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = m_rayGenPipelineInfo.shaderModule;
            pipelineInfo.stage.pName = "main";

            if (vkCreateComputePipelines(m_device.vkDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_rayGenPipelineInfo.pipeline) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create ray generation pipeline");
            }
        }

        // Create intersection pipeline
        {
            VkShaderModuleCreateInfo shaderModuleInfo{};
            shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderModuleInfo.codeSize = wavefrontCompilerResult.intersectShaderSpirV.size() * sizeof(uint32_t);
            shaderModuleInfo.pCode = wavefrontCompilerResult.intersectShaderSpirV.data();
            if (vkCreateShaderModule(m_device.vkDevice(), &shaderModuleInfo, nullptr, &m_intersectPipelineInfo.shaderModule) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create intersection shader module");
            }

            m_intersectPipelineInfo.pipelineLayout = pipelineLayout;
            m_intersectPipelineInfo.descriptorSetLayout = descriptorSetLayout;

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = m_intersectPipelineInfo.shaderModule;
            pipelineInfo.stage.pName = "main";

            if (vkCreateComputePipelines(m_device.vkDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_intersectPipelineInfo.pipeline) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create intersection pipeline");
            }
        }

        // Create hit shader pipelines
        m_hitPipelines.resize(wavefrontCompilerResult.hitShadersSpirV.size());
        for (size_t i = 0; i < wavefrontCompilerResult.hitShadersSpirV.size(); ++i)
        {
            VkShaderModuleCreateInfo shaderModuleInfo{};
            shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderModuleInfo.codeSize = wavefrontCompilerResult.hitShadersSpirV[i].size() * sizeof(uint32_t);
            shaderModuleInfo.pCode = wavefrontCompilerResult.hitShadersSpirV[i].data();
            if (vkCreateShaderModule(m_device.vkDevice(), &shaderModuleInfo, nullptr, &m_hitPipelines[i].shaderModule) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create hit shader module");
            }

            m_hitPipelines[i].pipelineLayout = pipelineLayout;
            m_hitPipelines[i].descriptorSetLayout = descriptorSetLayout;

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = m_hitPipelines[i].shaderModule;
            pipelineInfo.stage.pName = "main";

            if (vkCreateComputePipelines(m_device.vkDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_hitPipelines[i].pipeline) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create hit shader pipeline");
            }
        }

        // Create miss shader pipelines
        m_missPipelines.resize(wavefrontCompilerResult.missShadersSpirV.size());
        for (size_t i = 0; i < wavefrontCompilerResult.missShadersSpirV.size(); ++i)
        {
            VkShaderModuleCreateInfo shaderModuleInfo{};
            shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderModuleInfo.codeSize = wavefrontCompilerResult.missShadersSpirV[i].size() * sizeof(uint32_t);
            shaderModuleInfo.pCode = wavefrontCompilerResult.missShadersSpirV[i].data();
            if (vkCreateShaderModule(m_device.vkDevice(), &shaderModuleInfo, nullptr, &m_missPipelines[i].shaderModule) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create miss shader module");
            }

            m_missPipelines[i].pipelineLayout = pipelineLayout;
            m_missPipelines[i].descriptorSetLayout = descriptorSetLayout;

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = m_missPipelines[i].shaderModule;
            pipelineInfo.stage.pName = "main";

            if (vkCreateComputePipelines(m_device.vkDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_missPipelines[i].pipeline) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create miss shader pipeline");
            }
        }
    }

    VulkanWaveFrontPipeline::~VulkanWaveFrontPipeline()
    {
        auto device = m_device.vkDevice();

        // Destroy pipelines
        if (m_rayGenPipelineInfo.pipeline)
            vkDestroyPipeline(device, m_rayGenPipelineInfo.pipeline, nullptr);
        if (m_intersectPipelineInfo.pipeline)
            vkDestroyPipeline(device, m_intersectPipelineInfo.pipeline, nullptr);

        for (auto &hitPipeline : m_hitPipelines)
        {
            if (hitPipeline.pipeline)
                vkDestroyPipeline(device, hitPipeline.pipeline, nullptr);
        }

        for (auto &missPipeline : m_missPipelines)
        {
            if (missPipeline.pipeline)
                vkDestroyPipeline(device, missPipeline.pipeline, nullptr);
        }

        // Destroy shader modules
        if (m_rayGenPipelineInfo.shaderModule)
            vkDestroyShaderModule(device, m_rayGenPipelineInfo.shaderModule, nullptr);
        if (m_intersectPipelineInfo.shaderModule)
            vkDestroyShaderModule(device, m_intersectPipelineInfo.shaderModule, nullptr);

        for (auto &hitPipeline : m_hitPipelines)
        {
            if (hitPipeline.shaderModule)
                vkDestroyShaderModule(device, hitPipeline.shaderModule, nullptr);
        }

        for (auto &missPipeline : m_missPipelines)
        {
            if (missPipeline.shaderModule)
                vkDestroyShaderModule(device, missPipeline.shaderModule, nullptr);
        }

        // Destroy pipeline layout (shared, so only destroy once)
        if (m_rayGenPipelineInfo.pipelineLayout)
        {
            vkDestroyPipelineLayout(device, m_rayGenPipelineInfo.pipelineLayout, nullptr);
        }

        // Destroy descriptor set layout (shared, so only destroy once)
        if (m_rayGenPipelineInfo.descriptorSetLayout)
        {
            vkDestroyDescriptorSetLayout(device, m_rayGenPipelineInfo.descriptorSetLayout, nullptr);
        }
    }
    void VulkanWaveFrontPipeline::allocateDescriptorSets(std::span<DescriptorSet *> sets)
    {
        for (auto &set : sets)
        {
            // Reuse existing VulkanComputeRayTracingDescriptorSet
            // It already supports the binding pattern we need (TLAS + user bindings)
            auto vkSet = new VulkanComputeRayTracingDescriptorSet(m_device, m_layout, m_rayGenPipelineInfo.descriptorSetLayout);
            set = vkSet;
        }
    }
    size_t VulkanWaveFrontPipeline::bindingStartOffset(ShaderStage stage) const
    {
        switch (stage)
        {
        case ShaderStage::RayGeneration:
            return 1; // 0: path headers, 1: intersection queue
        case ShaderStage::Miss:
            return 2; // 0: path headers, 1: ray queue, 2: hit info
        case ShaderStage::ClosestHit:
            return 2; // 0: path headers, 1: ray queue, 2: hit info
        case ShaderStage::AnyHit:
            return 2; // 0: path headers, 1: ray queue, 2: hit info
        case ShaderStage::Intersection:
            return 0; // 0: TLAS (6 bindings)
        }
    }
}