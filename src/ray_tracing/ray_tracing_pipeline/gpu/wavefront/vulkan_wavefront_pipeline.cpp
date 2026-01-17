#include "vulkan_wavefront_pipeline.hpp"
#include "../vulkan_compute_pipeline_compiler.hpp"
#include "../vulkan_compute_raytracing_descriptor_set.hpp"
#include "../../ray_tracing_pipeline_layout.hpp"
#include "../../../../device/gpu/vulkan_compute_device.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <shaderc/shaderc.hpp>

namespace tracey
{
    VulkanWaveFrontPipeline::VulkanWaveFrontPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt) : m_device(device), m_layout(layout)
    {
        fprintf(stderr, "\n*** VulkanWaveFrontPipeline constructor called ***\n");
        fflush(stderr);
        const auto wavefrontCompilerResult = compileVulkanWaveFrontRayTracingPipeline(layout, sbt);
        fprintf(stderr, "*** Shader compilation complete ***\n");
        fflush(stderr);

        // Create descriptor set layout (shared by all pipelines)
        std::vector<VkDescriptorSetLayoutBinding> bindings;

        // Reserve first 6 bindings (0-5) for acceleration structure (TLAS)
        // User bindings start at 6
        const size_t bindingStartOffset = 6;

        // Add wavefront internal buffers at bindings 20 (payload), 50-52 (internal)
        // Payload buffer at binding 20
        bindings.push_back({.binding = 20,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // PathHeader buffer
        bindings.push_back({.binding = 50,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // RayQueue buffer
        bindings.push_back({.binding = 51,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // HitInfo buffer
        bindings.push_back({.binding = 52,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // NextRayQueue buffer (for ping-pong multi-bounce)
        bindings.push_back({.binding = 53,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // Indirect dispatch buffer
        bindings.push_back({.binding = 54,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // Hit queue buffer
        bindings.push_back({.binding = 55,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // Miss queue buffer
        bindings.push_back({.binding = 56,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // Hit indirect dispatch buffer
        bindings.push_back({.binding = 57,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // Miss indirect dispatch buffer
        bindings.push_back({.binding = 58,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // TLAS buffers (bindings 0-5) - always added for acceleration structure access
        for (size_t i = 0; i < 6; ++i)
        {
            bindings.push_back({.binding = static_cast<uint32_t>(i),
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                .descriptorCount = 1,
                                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});
        }

        // Add user-defined bindings (images, buffers, etc.)
        // User bindings start at offset 6 (after TLAS bindings 0-5)
        for (const auto &binding : layout.bindings())
        {
            const size_t bindingIndex = layout.indexForBinding(binding.name);
            fprintf(stderr, "*** User binding: %s, index=%zu, final binding=%zu\n", binding.name.c_str(), bindingIndex, bindingIndex + bindingStartOffset);
            fflush(stderr);
            VkDescriptorSetLayoutBinding vkBinding{};
            vkBinding.binding = bindingIndex + bindingStartOffset; // Apply offset to ALL user bindings
            vkBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            vkBinding.descriptorCount = 1;

            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::StorageBuffer:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::UniformBuffer:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
                // TLAS bindings already added above (0-5), skip here
                break;
            default:
                throw std::runtime_error("Unsupported descriptor type");
            }
        }

        VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
        descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorSetLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        descriptorSetLayoutInfo.pBindings = bindings.data();

        // Debug: print all bindings
        fprintf(stderr, "\n=== WAVEFRONT DESCRIPTOR SET LAYOUT DEBUG ===\n");
        fprintf(stderr, "Creating descriptor set layout with %zu bindings:\n", bindings.size());
        for (const auto &b : bindings)
        {
            fprintf(stderr, "  Binding %u: type=%u count=%u\n", b.binding, b.descriptorType, b.descriptorCount);
        }
        fprintf(stderr, "=============================================\n\n");
        fflush(stderr);

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

        // Create resolve shader pipeline
        VkShaderModuleCreateInfo resolveModuleInfo{};
        resolveModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        resolveModuleInfo.codeSize = wavefrontCompilerResult.resolveShaderSpirV.size() * sizeof(uint32_t);
        resolveModuleInfo.pCode = wavefrontCompilerResult.resolveShaderSpirV.data();
        if (vkCreateShaderModule(m_device.vkDevice(), &resolveModuleInfo, nullptr, &m_resolvePipelineInfo.shaderModule) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create resolve shader module");
        }

        m_resolvePipelineInfo.pipelineLayout = pipelineLayout;
        m_resolvePipelineInfo.descriptorSetLayout = descriptorSetLayout;

        VkComputePipelineCreateInfo resolvePipelineInfo{};
        resolvePipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        resolvePipelineInfo.layout = pipelineLayout;
        resolvePipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        resolvePipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        resolvePipelineInfo.stage.module = m_resolvePipelineInfo.shaderModule;
        resolvePipelineInfo.stage.pName = "main";

        if (vkCreateComputePipelines(m_device.vkDevice(), VK_NULL_HANDLE, 1, &resolvePipelineInfo, nullptr, &m_resolvePipelineInfo.pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create resolve shader pipeline");
        }

        // Create prepare indirect dispatch pipeline
        {
            // Load and compile the prepare indirect shader
            std::filesystem::path prepareIndirectPath = std::filesystem::path(__FILE__).parent_path() / "vulkan_wavefront_prepare_indirect.comp";
            std::ifstream prepareIndirectFile(prepareIndirectPath);
            if (!prepareIndirectFile.is_open())
            {
                throw std::runtime_error("Failed to open prepare indirect shader file: " + prepareIndirectPath.string());
            }
            std::stringstream prepareIndirectStream;
            prepareIndirectStream << prepareIndirectFile.rdbuf();
            std::string prepareIndirectSource = prepareIndirectStream.str();

            shaderc::Compiler compiler;
            shaderc::CompileOptions options;
            options.SetOptimizationLevel(shaderc_optimization_level_performance);
            options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);

            shaderc::SpvCompilationResult prepareIndirectModule = compiler.CompileGlslToSpv(
                prepareIndirectSource, shaderc_glsl_compute_shader, "vulkan_wavefront_prepare_indirect.comp", options);

            if (prepareIndirectModule.GetCompilationStatus() != shaderc_compilation_status_success)
            {
                throw std::runtime_error("Failed to compile prepare indirect shader: " + std::string(prepareIndirectModule.GetErrorMessage()));
            }
            std::vector<uint32_t> prepareIndirectSpirV(prepareIndirectModule.cbegin(), prepareIndirectModule.cend());

            VkShaderModuleCreateInfo prepareIndirectModuleInfo{};
            prepareIndirectModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            prepareIndirectModuleInfo.codeSize = prepareIndirectSpirV.size() * sizeof(uint32_t);
            prepareIndirectModuleInfo.pCode = prepareIndirectSpirV.data();
            if (vkCreateShaderModule(m_device.vkDevice(), &prepareIndirectModuleInfo, nullptr, &m_prepareIndirectPipelineInfo.shaderModule) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create prepare indirect shader module");
            }

            m_prepareIndirectPipelineInfo.pipelineLayout = pipelineLayout;
            m_prepareIndirectPipelineInfo.descriptorSetLayout = descriptorSetLayout;

            VkComputePipelineCreateInfo prepareIndirectPipelineInfo{};
            prepareIndirectPipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            prepareIndirectPipelineInfo.layout = pipelineLayout;
            prepareIndirectPipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            prepareIndirectPipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            prepareIndirectPipelineInfo.stage.module = m_prepareIndirectPipelineInfo.shaderModule;
            prepareIndirectPipelineInfo.stage.pName = "main";

            if (vkCreateComputePipelines(m_device.vkDevice(), VK_NULL_HANDLE, 1, &prepareIndirectPipelineInfo, nullptr, &m_prepareIndirectPipelineInfo.pipeline) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create prepare indirect pipeline");
            }
        }

        // Calculate payload size from layout
        m_payloadSize = 0;
        for (const auto &payload : layout.payloads())
        {
            m_payloadSize += payload.structure.size();
        }
        // Align to 16 bytes for std430 layout
        m_payloadSize = (m_payloadSize + 15) & ~15;

        // Allocate internal buffers with a default size (2048x2048 = 4194304 rays)
        // This will be reallocated if a larger resolution is needed
        allocateInternalBuffers(4194304);
    }

    VulkanWaveFrontPipeline::~VulkanWaveFrontPipeline()
    {
        auto device = m_device.vkDevice();

        // Destroy internal buffers
        if (m_payloadBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_payloadBuffer, nullptr);
        if (m_payloadMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_payloadMemory, nullptr);
        if (m_pathHeaderBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_pathHeaderBuffer, nullptr);
        if (m_pathHeaderMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_pathHeaderMemory, nullptr);
        if (m_rayQueueBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_rayQueueBuffer, nullptr);
        if (m_rayQueueMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_rayQueueMemory, nullptr);
        if (m_rayQueueBuffer2 != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_rayQueueBuffer2, nullptr);
        if (m_rayQueueMemory2 != VK_NULL_HANDLE)
            vkFreeMemory(device, m_rayQueueMemory2, nullptr);
        if (m_hitInfoBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_hitInfoBuffer, nullptr);
        if (m_hitInfoMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_hitInfoMemory, nullptr);
        if (m_hitQueueBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_hitQueueBuffer, nullptr);
        if (m_hitQueueMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_hitQueueMemory, nullptr);
        if (m_missQueueBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_missQueueBuffer, nullptr);
        if (m_missQueueMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_missQueueMemory, nullptr);
        if (m_indirectDispatchBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_indirectDispatchBuffer, nullptr);
        if (m_indirectDispatchMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_indirectDispatchMemory, nullptr);
        if (m_hitIndirectBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_hitIndirectBuffer, nullptr);
        if (m_hitIndirectMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_hitIndirectMemory, nullptr);
        if (m_missIndirectBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_missIndirectBuffer, nullptr);
        if (m_missIndirectMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_missIndirectMemory, nullptr);

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

        if (m_resolvePipelineInfo.pipeline)
            vkDestroyPipeline(device, m_resolvePipelineInfo.pipeline, nullptr);
        if (m_prepareIndirectPipelineInfo.pipeline)
            vkDestroyPipeline(device, m_prepareIndirectPipelineInfo.pipeline, nullptr);

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

        if (m_resolvePipelineInfo.shaderModule)
            vkDestroyShaderModule(device, m_resolvePipelineInfo.shaderModule, nullptr);
        if (m_prepareIndirectPipelineInfo.shaderModule)
            vkDestroyShaderModule(device, m_prepareIndirectPipelineInfo.shaderModule, nullptr);

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
        // For multi-bounce support, we need two descriptor sets with swapped ray queue bindings
        // Set 0: rayQueueBuffer at 51, rayQueueBuffer2 at 53
        // Set 1: rayQueueBuffer2 at 51, rayQueueBuffer at 53 (swapped for ping-pong)

        for (size_t i = 0; i < sets.size(); ++i)
        {
            // Reuse existing VulkanComputeRayTracingDescriptorSet with user binding offset of 6
            // (TLAS uses bindings 0-5, user bindings start at 6)
            auto vkSet = new VulkanComputeRayTracingDescriptorSet(m_device, m_layout, m_rayGenPipelineInfo.descriptorSetLayout, 6);
            sets[i] = vkSet;

            // Bind internal buffers if they're allocated
            if (m_pathHeaderBuffer != VK_NULL_HANDLE)
            {
                // For set 0: normal binding (rayQueueBuffer at 51, rayQueueBuffer2 at 53)
                // For set 1: swapped binding (rayQueueBuffer2 at 51, rayQueueBuffer at 53)
                bool swapQueues = (i % 2 == 1);
                bindInternalBuffers(vkSet->vkDescriptorSet(), swapQueues);
            }
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

    void VulkanWaveFrontPipeline::allocateInternalBuffers(uint32_t maxRayCount)
    {
        if (m_maxRayCount >= maxRayCount && m_pathHeaderBuffer != VK_NULL_HANDLE)
        {
            return; // Buffers already allocated with sufficient size
        }

        // Free existing buffers if reallocating
        auto device = m_device.vkDevice();
        if (m_pathHeaderBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, m_payloadBuffer, nullptr);
            vkFreeMemory(device, m_payloadMemory, nullptr);
            vkDestroyBuffer(device, m_pathHeaderBuffer, nullptr);
            vkFreeMemory(device, m_pathHeaderMemory, nullptr);
            vkDestroyBuffer(device, m_rayQueueBuffer, nullptr);
            vkFreeMemory(device, m_rayQueueMemory, nullptr);
            vkDestroyBuffer(device, m_rayQueueBuffer2, nullptr);
            vkFreeMemory(device, m_rayQueueMemory2, nullptr);
            vkDestroyBuffer(device, m_hitInfoBuffer, nullptr);
            vkFreeMemory(device, m_hitInfoMemory, nullptr);
            vkDestroyBuffer(device, m_hitQueueBuffer, nullptr);
            vkFreeMemory(device, m_hitQueueMemory, nullptr);
            vkDestroyBuffer(device, m_missQueueBuffer, nullptr);
            vkFreeMemory(device, m_missQueueMemory, nullptr);
            vkDestroyBuffer(device, m_indirectDispatchBuffer, nullptr);
            vkFreeMemory(device, m_indirectDispatchMemory, nullptr);
            vkDestroyBuffer(device, m_hitIndirectBuffer, nullptr);
            vkFreeMemory(device, m_hitIndirectMemory, nullptr);
            vkDestroyBuffer(device, m_missIndirectBuffer, nullptr);
            vkFreeMemory(device, m_missIndirectMemory, nullptr);
        }

        m_maxRayCount = maxRayCount;

        // Payload buffer: RayPayloads struct per ray
        VkDeviceSize payloadBufferSize = maxRayCount * m_payloadSize;

        VkBufferCreateInfo payloadBufferInfo{};
        payloadBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        payloadBufferInfo.size = payloadBufferSize;
        payloadBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        payloadBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &payloadBufferInfo, nullptr, &m_payloadBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Payload buffer");
        }

        VkMemoryRequirements payloadMemReqs;
        vkGetBufferMemoryRequirements(device, m_payloadBuffer, &payloadMemReqs);

        VkMemoryAllocateInfo payloadAllocInfo{};
        payloadAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        payloadAllocInfo.allocationSize = payloadMemReqs.size;
        payloadAllocInfo.memoryTypeIndex = m_device.findMemoryType(payloadMemReqs.memoryTypeBits,
                                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &payloadAllocInfo, nullptr, &m_payloadMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate Payload memory");
        }
        vkBindBufferMemory(device, m_payloadBuffer, m_payloadMemory, 0);

        // PathHeader buffer: 2 vec4s per ray (origin + direction with tMin/tMax)
        VkDeviceSize pathHeaderSize = maxRayCount * sizeof(float) * 8;

        VkBufferCreateInfo pathHeaderBufferInfo{};
        pathHeaderBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        pathHeaderBufferInfo.size = pathHeaderSize;
        pathHeaderBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        pathHeaderBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &pathHeaderBufferInfo, nullptr, &m_pathHeaderBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create PathHeader buffer");
        }

        VkMemoryRequirements pathHeaderMemReqs;
        vkGetBufferMemoryRequirements(device, m_pathHeaderBuffer, &pathHeaderMemReqs);

        VkMemoryAllocateInfo pathHeaderAllocInfo{};
        pathHeaderAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        pathHeaderAllocInfo.allocationSize = pathHeaderMemReqs.size;
        pathHeaderAllocInfo.memoryTypeIndex = m_device.findMemoryType(pathHeaderMemReqs.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &pathHeaderAllocInfo, nullptr, &m_pathHeaderMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate PathHeader memory");
        }
        vkBindBufferMemory(device, m_pathHeaderBuffer, m_pathHeaderMemory, 0);

        // RayQueue buffer: count (uint32) + indices (uint32 per ray)
        VkDeviceSize rayQueueSize = (sizeof(uint32_t) * 4) + maxRayCount * sizeof(uint32_t);

        VkBufferCreateInfo rayQueueBufferInfo{};
        rayQueueBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        rayQueueBufferInfo.size = rayQueueSize;
        rayQueueBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        rayQueueBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &rayQueueBufferInfo, nullptr, &m_rayQueueBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create RayQueue buffer");
        }

        VkMemoryRequirements rayQueueMemReqs;
        vkGetBufferMemoryRequirements(device, m_rayQueueBuffer, &rayQueueMemReqs);

        VkMemoryAllocateInfo rayQueueAllocInfo{};
        rayQueueAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        rayQueueAllocInfo.allocationSize = rayQueueMemReqs.size;
        rayQueueAllocInfo.memoryTypeIndex = m_device.findMemoryType(rayQueueMemReqs.memoryTypeBits,
                                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &rayQueueAllocInfo, nullptr, &m_rayQueueMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate RayQueue memory");
        }
        vkBindBufferMemory(device, m_rayQueueBuffer, m_rayQueueMemory, 0);

        // RayQueue2 buffer (for ping-pong): count (uint32) + indices (uint32 per ray)
        VkBufferCreateInfo rayQueue2BufferInfo{};
        rayQueue2BufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        rayQueue2BufferInfo.size = rayQueueSize;
        rayQueue2BufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        rayQueue2BufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &rayQueue2BufferInfo, nullptr, &m_rayQueueBuffer2) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create RayQueue2 buffer");
        }

        VkMemoryRequirements rayQueue2MemReqs;
        vkGetBufferMemoryRequirements(device, m_rayQueueBuffer2, &rayQueue2MemReqs);

        VkMemoryAllocateInfo rayQueue2AllocInfo{};
        rayQueue2AllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        rayQueue2AllocInfo.allocationSize = rayQueue2MemReqs.size;
        rayQueue2AllocInfo.memoryTypeIndex = m_device.findMemoryType(rayQueue2MemReqs.memoryTypeBits,
                                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &rayQueue2AllocInfo, nullptr, &m_rayQueueMemory2) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate RayQueue2 memory");
        }
        vkBindBufferMemory(device, m_rayQueueBuffer2, m_rayQueueMemory2, 0);

        // HitInfo buffer: t + triangleIndex + instanceIndex + barycentrics (3 floats + 2 uints per ray + padding)
        VkDeviceSize hitInfoSize = maxRayCount * (sizeof(float) * 8);

        VkBufferCreateInfo hitInfoBufferInfo{};
        hitInfoBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        hitInfoBufferInfo.size = hitInfoSize;
        hitInfoBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        hitInfoBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &hitInfoBufferInfo, nullptr, &m_hitInfoBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create HitInfo buffer");
        }

        VkMemoryRequirements hitInfoMemReqs;
        vkGetBufferMemoryRequirements(device, m_hitInfoBuffer, &hitInfoMemReqs);

        VkMemoryAllocateInfo hitInfoAllocInfo{};
        hitInfoAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        hitInfoAllocInfo.allocationSize = hitInfoMemReqs.size;
        hitInfoAllocInfo.memoryTypeIndex = m_device.findMemoryType(hitInfoMemReqs.memoryTypeBits,
                                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &hitInfoAllocInfo, nullptr, &m_hitInfoMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate HitInfo memory");
        }
        vkBindBufferMemory(device, m_hitInfoBuffer, m_hitInfoMemory, 0);

        // Indirect dispatch buffer: VkDispatchIndirectCommand (3 uint32s)
        VkDeviceSize indirectDispatchSize = sizeof(uint32_t) * 3;

        VkBufferCreateInfo indirectDispatchBufferInfo{};
        indirectDispatchBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        indirectDispatchBufferInfo.size = indirectDispatchSize;
        indirectDispatchBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        indirectDispatchBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &indirectDispatchBufferInfo, nullptr, &m_indirectDispatchBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create IndirectDispatch buffer");
        }

        VkMemoryRequirements indirectDispatchMemReqs;
        vkGetBufferMemoryRequirements(device, m_indirectDispatchBuffer, &indirectDispatchMemReqs);

        VkMemoryAllocateInfo indirectDispatchAllocInfo{};
        indirectDispatchAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        indirectDispatchAllocInfo.allocationSize = indirectDispatchMemReqs.size;
        indirectDispatchAllocInfo.memoryTypeIndex = m_device.findMemoryType(indirectDispatchMemReqs.memoryTypeBits,
                                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &indirectDispatchAllocInfo, nullptr, &m_indirectDispatchMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate IndirectDispatch memory");
        }
        vkBindBufferMemory(device, m_indirectDispatchBuffer, m_indirectDispatchMemory, 0);

        // Hit queue buffer: count (uint32) + indices (uint32 per ray)
        VkBufferCreateInfo hitQueueBufferInfo{};
        hitQueueBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        hitQueueBufferInfo.size = rayQueueSize;
        hitQueueBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        hitQueueBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &hitQueueBufferInfo, nullptr, &m_hitQueueBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create HitQueue buffer");
        }

        VkMemoryRequirements hitQueueMemReqs;
        vkGetBufferMemoryRequirements(device, m_hitQueueBuffer, &hitQueueMemReqs);

        VkMemoryAllocateInfo hitQueueAllocInfo{};
        hitQueueAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        hitQueueAllocInfo.allocationSize = hitQueueMemReqs.size;
        hitQueueAllocInfo.memoryTypeIndex = m_device.findMemoryType(hitQueueMemReqs.memoryTypeBits,
                                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &hitQueueAllocInfo, nullptr, &m_hitQueueMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate HitQueue memory");
        }
        vkBindBufferMemory(device, m_hitQueueBuffer, m_hitQueueMemory, 0);

        // Miss queue buffer: count (uint32) + indices (uint32 per ray)
        VkBufferCreateInfo missQueueBufferInfo{};
        missQueueBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        missQueueBufferInfo.size = rayQueueSize;
        missQueueBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        missQueueBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &missQueueBufferInfo, nullptr, &m_missQueueBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create MissQueue buffer");
        }

        VkMemoryRequirements missQueueMemReqs;
        vkGetBufferMemoryRequirements(device, m_missQueueBuffer, &missQueueMemReqs);

        VkMemoryAllocateInfo missQueueAllocInfo{};
        missQueueAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        missQueueAllocInfo.allocationSize = missQueueMemReqs.size;
        missQueueAllocInfo.memoryTypeIndex = m_device.findMemoryType(missQueueMemReqs.memoryTypeBits,
                                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &missQueueAllocInfo, nullptr, &m_missQueueMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate MissQueue memory");
        }
        vkBindBufferMemory(device, m_missQueueBuffer, m_missQueueMemory, 0);

        // Hit indirect dispatch buffer
        VkBufferCreateInfo hitIndirectBufferInfo{};
        hitIndirectBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        hitIndirectBufferInfo.size = indirectDispatchSize;
        hitIndirectBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        hitIndirectBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &hitIndirectBufferInfo, nullptr, &m_hitIndirectBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create HitIndirect buffer");
        }

        VkMemoryRequirements hitIndirectMemReqs;
        vkGetBufferMemoryRequirements(device, m_hitIndirectBuffer, &hitIndirectMemReqs);

        VkMemoryAllocateInfo hitIndirectAllocInfo{};
        hitIndirectAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        hitIndirectAllocInfo.allocationSize = hitIndirectMemReqs.size;
        hitIndirectAllocInfo.memoryTypeIndex = m_device.findMemoryType(hitIndirectMemReqs.memoryTypeBits,
                                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &hitIndirectAllocInfo, nullptr, &m_hitIndirectMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate HitIndirect memory");
        }
        vkBindBufferMemory(device, m_hitIndirectBuffer, m_hitIndirectMemory, 0);

        // Miss indirect dispatch buffer
        VkBufferCreateInfo missIndirectBufferInfo{};
        missIndirectBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        missIndirectBufferInfo.size = indirectDispatchSize;
        missIndirectBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        missIndirectBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &missIndirectBufferInfo, nullptr, &m_missIndirectBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create MissIndirect buffer");
        }

        VkMemoryRequirements missIndirectMemReqs;
        vkGetBufferMemoryRequirements(device, m_missIndirectBuffer, &missIndirectMemReqs);

        VkMemoryAllocateInfo missIndirectAllocInfo{};
        missIndirectAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        missIndirectAllocInfo.allocationSize = missIndirectMemReqs.size;
        missIndirectAllocInfo.memoryTypeIndex = m_device.findMemoryType(missIndirectMemReqs.memoryTypeBits,
                                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &missIndirectAllocInfo, nullptr, &m_missIndirectMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate MissIndirect memory");
        }
        vkBindBufferMemory(device, m_missIndirectBuffer, m_missIndirectMemory, 0);
    }

    void VulkanWaveFrontPipeline::bindInternalBuffers(VkDescriptorSet descriptorSet, bool swapQueues)
    {
        VkWriteDescriptorSet writes[10]{};

        // Binding 20: Payload buffer
        VkDescriptorBufferInfo payloadInfo{};
        payloadInfo.buffer = m_payloadBuffer;
        payloadInfo.offset = 0;
        payloadInfo.range = VK_WHOLE_SIZE;

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 20;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &payloadInfo;

        // Binding 50: PathHeader buffer
        VkDescriptorBufferInfo pathHeaderInfo{};
        pathHeaderInfo.buffer = m_pathHeaderBuffer;
        pathHeaderInfo.offset = 0;
        pathHeaderInfo.range = VK_WHOLE_SIZE;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet;
        writes[1].dstBinding = 50;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &pathHeaderInfo;

        // Binding 51: RayQueue buffer (current queue)
        VkDescriptorBufferInfo rayQueue1Info{};
        rayQueue1Info.buffer = swapQueues ? m_rayQueueBuffer2 : m_rayQueueBuffer;
        rayQueue1Info.offset = 0;
        rayQueue1Info.range = VK_WHOLE_SIZE;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet;
        writes[2].dstBinding = 51;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo = &rayQueue1Info;

        // Binding 52: HitInfo buffer
        VkDescriptorBufferInfo hitInfoInfo{};
        hitInfoInfo.buffer = m_hitInfoBuffer;
        hitInfoInfo.offset = 0;
        hitInfoInfo.range = VK_WHOLE_SIZE;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = descriptorSet;
        writes[3].dstBinding = 52;
        writes[3].dstArrayElement = 0;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].descriptorCount = 1;
        writes[3].pBufferInfo = &hitInfoInfo;

        // Binding 53: RayQueue2 buffer (next queue)
        VkDescriptorBufferInfo rayQueue2Info{};
        rayQueue2Info.buffer = swapQueues ? m_rayQueueBuffer : m_rayQueueBuffer2;
        rayQueue2Info.offset = 0;
        rayQueue2Info.range = VK_WHOLE_SIZE;

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = descriptorSet;
        writes[4].dstBinding = 53;
        writes[4].dstArrayElement = 0;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].descriptorCount = 1;
        writes[4].pBufferInfo = &rayQueue2Info;

        // Binding 54: Indirect dispatch buffer
        VkDescriptorBufferInfo indirectDispatchInfo{};
        indirectDispatchInfo.buffer = m_indirectDispatchBuffer;
        indirectDispatchInfo.offset = 0;
        indirectDispatchInfo.range = VK_WHOLE_SIZE;

        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = descriptorSet;
        writes[5].dstBinding = 54;
        writes[5].dstArrayElement = 0;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5].descriptorCount = 1;
        writes[5].pBufferInfo = &indirectDispatchInfo;

        // Binding 55: Hit queue buffer
        VkDescriptorBufferInfo hitQueueInfo{};
        hitQueueInfo.buffer = m_hitQueueBuffer;
        hitQueueInfo.offset = 0;
        hitQueueInfo.range = VK_WHOLE_SIZE;

        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = descriptorSet;
        writes[6].dstBinding = 55;
        writes[6].dstArrayElement = 0;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[6].descriptorCount = 1;
        writes[6].pBufferInfo = &hitQueueInfo;

        // Binding 56: Miss queue buffer
        VkDescriptorBufferInfo missQueueInfo{};
        missQueueInfo.buffer = m_missQueueBuffer;
        missQueueInfo.offset = 0;
        missQueueInfo.range = VK_WHOLE_SIZE;

        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = descriptorSet;
        writes[7].dstBinding = 56;
        writes[7].dstArrayElement = 0;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[7].descriptorCount = 1;
        writes[7].pBufferInfo = &missQueueInfo;

        // Binding 57: Hit indirect dispatch buffer
        VkDescriptorBufferInfo hitIndirectInfo{};
        hitIndirectInfo.buffer = m_hitIndirectBuffer;
        hitIndirectInfo.offset = 0;
        hitIndirectInfo.range = VK_WHOLE_SIZE;

        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = descriptorSet;
        writes[8].dstBinding = 57;
        writes[8].dstArrayElement = 0;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[8].descriptorCount = 1;
        writes[8].pBufferInfo = &hitIndirectInfo;

        // Binding 58: Miss indirect dispatch buffer
        VkDescriptorBufferInfo missIndirectInfo{};
        missIndirectInfo.buffer = m_missIndirectBuffer;
        missIndirectInfo.offset = 0;
        missIndirectInfo.range = VK_WHOLE_SIZE;

        writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[9].dstSet = descriptorSet;
        writes[9].dstBinding = 58;
        writes[9].dstArrayElement = 0;
        writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[9].descriptorCount = 1;
        writes[9].pBufferInfo = &missIndirectInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 10, writes, 0, nullptr);
    }
}