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

        // Reserve first 6 bindings (0-5) for acceleration structure (TLAS)
        // User bindings start at 6
        const size_t bindingStartOffset = 6;

        // Add wavefront internal buffers at bindings 50-52
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
                // TLAS requires 6 storage buffers at fixed bindings 0-5 (ignore bindingIndex from layout)
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                vkBinding.descriptorCount = 1;
                for (size_t i = 0; i < 6; ++i)
                {
                    vkBinding.binding = i; // Always use 0-5 for TLAS
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

        // Allocate internal buffers with a default size (2048x2048 = 4194304 rays)
        // This will be reallocated if a larger resolution is needed
        allocateInternalBuffers(4194304);
    }

    VulkanWaveFrontPipeline::~VulkanWaveFrontPipeline()
    {
        auto device = m_device.vkDevice();

        // Destroy internal buffers
        if (m_pathHeaderBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_pathHeaderBuffer, nullptr);
        if (m_pathHeaderMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_pathHeaderMemory, nullptr);
        if (m_rayQueueBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_rayQueueBuffer, nullptr);
        if (m_rayQueueMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_rayQueueMemory, nullptr);
        if (m_hitInfoBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_hitInfoBuffer, nullptr);
        if (m_hitInfoMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, m_hitInfoMemory, nullptr);

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
            // Reuse existing VulkanComputeRayTracingDescriptorSet with user binding offset of 6
            // (TLAS uses bindings 0-5, user bindings start at 6)
            auto vkSet = new VulkanComputeRayTracingDescriptorSet(m_device, m_layout, m_rayGenPipelineInfo.descriptorSetLayout, 6);
            set = vkSet;

            // Bind internal buffers if they're allocated
            if (m_pathHeaderBuffer != VK_NULL_HANDLE)
            {
                bindInternalBuffers(vkSet->vkDescriptorSet());
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
            vkDestroyBuffer(device, m_pathHeaderBuffer, nullptr);
            vkFreeMemory(device, m_pathHeaderMemory, nullptr);
            vkDestroyBuffer(device, m_rayQueueBuffer, nullptr);
            vkFreeMemory(device, m_rayQueueMemory, nullptr);
            vkDestroyBuffer(device, m_hitInfoBuffer, nullptr);
            vkFreeMemory(device, m_hitInfoMemory, nullptr);
        }

        m_maxRayCount = maxRayCount;

        // PathHeader buffer: 2 vec4s per ray (origin + direction with tMin/tMax)
        VkDeviceSize pathHeaderSize = maxRayCount * sizeof(float) * 8;

        VkBufferCreateInfo pathHeaderBufferInfo{};
        pathHeaderBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        pathHeaderBufferInfo.size = pathHeaderSize;
        pathHeaderBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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
        rayQueueBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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

        // HitInfo buffer: t + triangleIndex + instanceIndex + barycentrics (5 floats + 2 uints per ray)
        VkDeviceSize hitInfoSize = maxRayCount * (sizeof(float) + sizeof(uint32_t) * 2 + sizeof(float) * 2);

        VkBufferCreateInfo hitInfoBufferInfo{};
        hitInfoBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        hitInfoBufferInfo.size = hitInfoSize;
        hitInfoBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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
    }

    void VulkanWaveFrontPipeline::bindInternalBuffers(VkDescriptorSet descriptorSet)
    {
        VkWriteDescriptorSet writes[3]{};

        // Binding 50: PathHeader buffer
        VkDescriptorBufferInfo pathHeaderInfo{};
        pathHeaderInfo.buffer = m_pathHeaderBuffer;
        pathHeaderInfo.offset = 0;
        pathHeaderInfo.range = VK_WHOLE_SIZE;

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 50;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &pathHeaderInfo;

        // Binding 51: RayQueue buffer
        VkDescriptorBufferInfo rayQueueInfo{};
        rayQueueInfo.buffer = m_rayQueueBuffer;
        rayQueueInfo.offset = 0;
        rayQueueInfo.range = VK_WHOLE_SIZE;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet;
        writes[1].dstBinding = 51;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &rayQueueInfo;

        // Binding 52: HitInfo buffer
        VkDescriptorBufferInfo hitInfoInfo{};
        hitInfoInfo.buffer = m_hitInfoBuffer;
        hitInfoInfo.offset = 0;
        hitInfoInfo.range = VK_WHOLE_SIZE;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet;
        writes[2].dstBinding = 52;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo = &hitInfoInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 3, writes, 0, nullptr);
    }
}