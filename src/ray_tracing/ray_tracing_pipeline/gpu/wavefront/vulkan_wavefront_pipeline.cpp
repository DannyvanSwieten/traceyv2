#include "vulkan_wavefront_pipeline.hpp"
#include "../vulkan_compute_pipeline_compiler.hpp"
#include "../vulkan_compute_raytracing_descriptor_set.hpp"
#include "../../ray_tracing_pipeline_layout.hpp"
#include "../../../../device/gpu/vulkan_compute_device.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <shaderc/shaderc.hpp>

namespace tracey
{
    VulkanWaveFrontPipeline::VulkanWaveFrontPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt) : m_device(device), m_layout(layout)
    {
        const auto wavefrontCompilerResult = compileVulkanWaveFrontRayTracingPipeline(layout, sbt);

        // Create descriptor set layout (shared by all pipelines)
        std::vector<VkDescriptorSetLayoutBinding> bindings;

        // Reserve first 8 bindings (0-7) for acceleration structure (TLAS + BVH)
        // User bindings start at 8
        const size_t bindingStartOffset = 8;

        // Add wavefront internal buffers: payload at 60, queues 50-58.
        // Payload moved out of the user binding range (was 20) so user SSBOs
        // (e.g. material program buffers) can extend past binding 19 safely.
        bindings.push_back({.binding = 60,
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

        // Sorted hit queue (output of the material-ID sort pass)
        bindings.push_back({.binding = 59,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // Material bin offsets (one uint per bin; written by sort_count, read by sort_scatter)
        bindings.push_back({.binding = 61,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // Material bin cursors (atomic write cursors used by sort_scatter, cleared per bounce)
        bindings.push_back({.binding = 62,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});

        // TLAS buffers (bindings 0-7) - always added for acceleration structure access
        // 0-5: existing TLAS data, 6-7: TLAS BVH nodes and instance indices
        for (size_t i = 0; i < 8; ++i)
        {
            bindings.push_back({.binding = static_cast<uint32_t>(i),
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                .descriptorCount = 1,
                                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});
        }

        // Add user-defined bindings (images, buffers, etc.)
        // User bindings start at offset 8 (after TLAS bindings 0-7)
        for (const auto &binding : layout.bindings())
        {
            const size_t bindingIndex = layout.indexForBinding(binding.name);
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
                // TLAS bindings already added above (0-7), skip here
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::SampledTextureArray:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                vkBinding.descriptorCount = binding.textureArrayCount;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Sampler:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                vkBinding.descriptorCount = 1;
                bindings.push_back(vkBinding);
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::SampledImageArray:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                vkBinding.descriptorCount = binding.textureArrayCount;
                bindings.push_back(vkBinding);
                break;
            default:
                throw std::runtime_error("Unsupported descriptor type");
            }
        }

        // Sort bindings by binding number to ensure texture array (with variable count) is last
        // This is required for VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
        std::sort(bindings.begin(), bindings.end(),
                  [](const VkDescriptorSetLayoutBinding& a, const VkDescriptorSetLayoutBinding& b) {
                      return a.binding < b.binding;
                  });

        // Descriptor set layout without bindless support for now
        // TODO: Re-enable bindless texture support once binding order issues are resolved
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
        pushConstantRange.size = sizeof(uint32_t) * 4; // width, height, sampleIndex, samplesPerFrame

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

        // Material-ID sort kernels (sort_count + sort_scatter). Loaded inline
        // exactly the same way prepare_indirect is -- they're internal to the
        // wavefront pipeline and don't go through the user shader builder.
        //
        // The sort kernels need to read instanceData (a user-side SSBO whose
        // binding number depends on declaration order); we look it up here
        // and inject as a #define so the kernel can declare a layout
        // qualifier with the right number. instanceData packs (programId,
        // uvOffset) per TLAS instance into a uvec2[].
        const uint32_t instanceDataBinding =
            static_cast<uint32_t>(layout.indexForBinding("instanceData") + 8);

        auto compileInternal = [&](const char *fileName, PipelineInfo &info) {
            std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / fileName;
            std::ifstream file(path);
            if (!file.is_open())
                throw std::runtime_error(std::string("Failed to open shader file: ") + path.string());
            std::stringstream stream;
            stream << file.rdbuf();
            std::string source = stream.str();

            // Inject the dynamic binding number right after #version so the
            // kernel can `layout(set = 0, binding = INSTANCE_DATA_BINDING) ...`.
            const std::string define =
                "\n#define INSTANCE_DATA_BINDING " +
                std::to_string(instanceDataBinding) + "\n";
            const size_t versionLineEnd = source.find('\n', source.find("#version"));
            if (versionLineEnd != std::string::npos)
                source.insert(versionLineEnd + 1, define);
            else
                source = define + source;

            shaderc::Compiler compiler;
            shaderc::CompileOptions options;
            options.SetOptimizationLevel(shaderc_optimization_level_performance);
            options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);

            shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
                source, shaderc_glsl_compute_shader, fileName, options);
            if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                throw std::runtime_error(std::string("Failed to compile ") + fileName + ": " + result.GetErrorMessage());

            std::vector<uint32_t> spirv(result.cbegin(), result.cend());

            VkShaderModuleCreateInfo moduleInfo{};
            moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
            moduleInfo.pCode = spirv.data();
            if (vkCreateShaderModule(m_device.vkDevice(), &moduleInfo, nullptr, &info.shaderModule) != VK_SUCCESS)
                throw std::runtime_error(std::string("Failed to create shader module for ") + fileName);

            info.pipelineLayout = pipelineLayout;
            info.descriptorSetLayout = descriptorSetLayout;

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = info.shaderModule;
            pipelineInfo.stage.pName = "main";
            if (vkCreateComputePipelines(m_device.vkDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &info.pipeline) != VK_SUCCESS)
                throw std::runtime_error(std::string("Failed to create pipeline for ") + fileName);
        };
        compileInternal("vulkan_wavefront_sort_count.comp", m_sortCountPipelineInfo);
        compileInternal("vulkan_wavefront_sort_scatter.comp", m_sortScatterPipelineInfo);

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

        destroyInternalBuffers();

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
        if (m_sortCountPipelineInfo.pipeline)
            vkDestroyPipeline(device, m_sortCountPipelineInfo.pipeline, nullptr);
        if (m_sortScatterPipelineInfo.pipeline)
            vkDestroyPipeline(device, m_sortScatterPipelineInfo.pipeline, nullptr);

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
        if (m_sortCountPipelineInfo.shaderModule)
            vkDestroyShaderModule(device, m_sortCountPipelineInfo.shaderModule, nullptr);
        if (m_sortScatterPipelineInfo.shaderModule)
            vkDestroyShaderModule(device, m_sortScatterPipelineInfo.shaderModule, nullptr);

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
            // Reuse existing VulkanComputeRayTracingDescriptorSet with user binding offset of 8
            // (TLAS uses bindings 0-7 including BVH, user bindings start at 8)
            auto vkSet = new VulkanComputeRayTracingDescriptorSet(m_device, m_layout, m_rayGenPipelineInfo.descriptorSetLayout, 8);
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

    void VulkanWaveFrontPipeline::destroyInternalBuffers()
    {
        auto device = m_device.vkDevice();

        VkBuffer *buffers[] = {
            &m_payloadBuffer, &m_pathHeaderBuffer, &m_rayQueueBuffer, &m_rayQueueBuffer2,
            &m_hitInfoBuffer, &m_hitQueueBuffer, &m_missQueueBuffer, &m_indirectDispatchBuffer,
            &m_hitIndirectBuffer, &m_missIndirectBuffer, &m_sortedHitQueueBuffer,
            &m_materialBinOffsetsBuffer, &m_materialBinCursorsBuffer};
        VkDeviceMemory *memories[] = {
            &m_payloadMemory, &m_pathHeaderMemory, &m_rayQueueMemory, &m_rayQueueMemory2,
            &m_hitInfoMemory, &m_hitQueueMemory, &m_missQueueMemory, &m_indirectDispatchMemory,
            &m_hitIndirectMemory, &m_missIndirectMemory, &m_sortedHitQueueMemory,
            &m_materialBinOffsetsMemory, &m_materialBinCursorsMemory};

        for (size_t i = 0; i < std::size(buffers); ++i)
        {
            if (*buffers[i] != VK_NULL_HANDLE)
                vkDestroyBuffer(device, *buffers[i], nullptr);
            if (*memories[i] != VK_NULL_HANDLE)
                vkFreeMemory(device, *memories[i], nullptr);
            *buffers[i] = VK_NULL_HANDLE;
            *memories[i] = VK_NULL_HANDLE;
        }
    }

    void VulkanWaveFrontPipeline::createDeviceLocalBuffer(VkBuffer &buffer, VkDeviceMemory &memory,
                                                          VkDeviceSize size, VkBufferUsageFlags usage,
                                                          const char *what)
    {
        auto device = m_device.vkDevice();

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (VkResult res = vkCreateBuffer(device, &info, nullptr, &buffer); res != VK_SUCCESS)
            throw std::runtime_error(std::string("Failed to create buffer ") + what +
                                     " (VkResult " + std::to_string(res) + ")");

        VkMemoryRequirements reqs;
        vkGetBufferMemoryRequirements(device, buffer, &reqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = reqs.size;
        allocInfo.memoryTypeIndex = m_device.findMemoryType(reqs.memoryTypeBits,
                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (VkResult res = vkAllocateMemory(device, &allocInfo, nullptr, &memory); res != VK_SUCCESS)
            throw std::runtime_error(std::string("Failed to allocate memory for ") + what +
                                     " (VkResult " + std::to_string(res) + ")");

        if (VkResult res = vkBindBufferMemory(device, buffer, memory, 0); res != VK_SUCCESS)
            throw std::runtime_error(std::string("Failed to bind memory for ") + what +
                                     " (VkResult " + std::to_string(res) + ")");
    }

    void VulkanWaveFrontPipeline::allocateInternalBuffers(uint32_t maxRayCount)
    {
        if (m_maxRayCount >= maxRayCount && m_pathHeaderBuffer != VK_NULL_HANDLE)
        {
            return; // Buffers already allocated with sufficient size
        }

        destroyInternalBuffers();
        m_maxRayCount = maxRayCount;

        constexpr VkBufferUsageFlags kStorage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        constexpr VkBufferUsageFlags kIndirect =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

        // Queue buffers: count header (4 uint32s) + one index per ray.
        const VkDeviceSize rayQueueSize = (sizeof(uint32_t) * 4) + maxRayCount * sizeof(uint32_t);
        const VkDeviceSize indirectSize = sizeof(uint32_t) * 3; // VkDispatchIndirectCommand
        // Material bin offsets/cursors: NUM_BINS uints each, matching NUM_BINS in the
        // sort kernels. Tiny fixed size -- they don't scale with the framebuffer.
        constexpr uint32_t kNumBins = 64;
        const VkDeviceSize binSize = sizeof(uint32_t) * kNumBins;

        try
        {
            createDeviceLocalBuffer(m_payloadBuffer, m_payloadMemory,
                                    VkDeviceSize(maxRayCount) * m_payloadSize, kStorage, "Payload");
            // PathHeader: 2 vec4s per ray (origin + direction with tMin/tMax)
            createDeviceLocalBuffer(m_pathHeaderBuffer, m_pathHeaderMemory,
                                    VkDeviceSize(maxRayCount) * sizeof(float) * 8, kStorage, "PathHeader");
            createDeviceLocalBuffer(m_rayQueueBuffer, m_rayQueueMemory, rayQueueSize, kStorage, "RayQueue");
            // Ping-pong partner of RayQueue
            createDeviceLocalBuffer(m_rayQueueBuffer2, m_rayQueueMemory2, rayQueueSize, kStorage, "RayQueue2");
            // HitInfo: t + triangleIndex + instanceIndex + barycentrics (3 floats + 2 uints per ray + padding)
            createDeviceLocalBuffer(m_hitInfoBuffer, m_hitInfoMemory,
                                    VkDeviceSize(maxRayCount) * sizeof(float) * 8, kStorage, "HitInfo");
            createDeviceLocalBuffer(m_indirectDispatchBuffer, m_indirectDispatchMemory,
                                    indirectSize, kIndirect, "IndirectDispatch");
            createDeviceLocalBuffer(m_hitQueueBuffer, m_hitQueueMemory, rayQueueSize, kStorage, "HitQueue");
            createDeviceLocalBuffer(m_missQueueBuffer, m_missQueueMemory, rayQueueSize, kStorage, "MissQueue");
            createDeviceLocalBuffer(m_hitIndirectBuffer, m_hitIndirectMemory,
                                    indirectSize, kIndirect, "HitIndirect");
            createDeviceLocalBuffer(m_missIndirectBuffer, m_missIndirectMemory,
                                    indirectSize, kIndirect, "MissIndirect");
            // Output of the material-ID sort, consumed by the hit shader.
            createDeviceLocalBuffer(m_sortedHitQueueBuffer, m_sortedHitQueueMemory,
                                    rayQueueSize, kStorage, "SortedHitQueue");
            createDeviceLocalBuffer(m_materialBinOffsetsBuffer, m_materialBinOffsetsMemory,
                                    binSize, kStorage, "MaterialBinOffsets");
            createDeviceLocalBuffer(m_materialBinCursorsBuffer, m_materialBinCursorsMemory,
                                    binSize, kStorage, "MaterialBinCursors");
        }
        catch (...)
        {
            destroyInternalBuffers();
            m_maxRayCount = 0;
            throw;
        }
    }

    void VulkanWaveFrontPipeline::bindInternalBuffers(VkDescriptorSet descriptorSet, bool swapQueues)
    {
        VkWriteDescriptorSet writes[13]{};

        // Binding 60: Payload buffer
        VkDescriptorBufferInfo payloadInfo{};
        payloadInfo.buffer = m_payloadBuffer;
        payloadInfo.offset = 0;
        payloadInfo.range = VK_WHOLE_SIZE;

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 60;
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

        // Binding 59: Sorted hit queue (output of the material-ID sort)
        VkDescriptorBufferInfo sortedHitQueueInfo{};
        sortedHitQueueInfo.buffer = m_sortedHitQueueBuffer;
        sortedHitQueueInfo.offset = 0;
        sortedHitQueueInfo.range = VK_WHOLE_SIZE;

        writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[10].dstSet = descriptorSet;
        writes[10].dstBinding = 59;
        writes[10].dstArrayElement = 0;
        writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[10].descriptorCount = 1;
        writes[10].pBufferInfo = &sortedHitQueueInfo;

        // Binding 61: Material bin offsets
        VkDescriptorBufferInfo materialBinOffsetsInfo{};
        materialBinOffsetsInfo.buffer = m_materialBinOffsetsBuffer;
        materialBinOffsetsInfo.offset = 0;
        materialBinOffsetsInfo.range = VK_WHOLE_SIZE;

        writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[11].dstSet = descriptorSet;
        writes[11].dstBinding = 61;
        writes[11].dstArrayElement = 0;
        writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[11].descriptorCount = 1;
        writes[11].pBufferInfo = &materialBinOffsetsInfo;

        // Binding 62: Material bin cursors
        VkDescriptorBufferInfo materialBinCursorsInfo{};
        materialBinCursorsInfo.buffer = m_materialBinCursorsBuffer;
        materialBinCursorsInfo.offset = 0;
        materialBinCursorsInfo.range = VK_WHOLE_SIZE;

        writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[12].dstSet = descriptorSet;
        writes[12].dstBinding = 62;
        writes[12].dstArrayElement = 0;
        writes[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[12].descriptorCount = 1;
        writes[12].pBufferInfo = &materialBinCursorsInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 13, writes, 0, nullptr);
    }
}