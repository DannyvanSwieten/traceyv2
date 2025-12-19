#include "vulkan_compute_raytracing_descriptor_set.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include "../../../device/gpu/vulkan_compute_device.hpp"
#include "../../../device/gpu/vulkan_image_2d.hpp"
#include "../../../device/gpu/vulkan_buffer.hpp"
#include "../../../device/gpu/vulkan_compute_top_level_acceleration_structure.hpp"
#include <stdexcept>

namespace tracey
{
    VulkanComputeRayTracingDescriptorSet::VulkanComputeRayTracingDescriptorSet(VulkanComputeDevice &device, const RayTracingPipelineLayout &layout) : m_device(device)
    {
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
            case RayTracingPipelineLayout::DescriptorType::Image2D:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                break;
            case RayTracingPipelineLayout::DescriptorType::Buffer:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                break;
            case RayTracingPipelineLayout::DescriptorType::AccelerationStructure:
                vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                break;
            default:
                throw std::runtime_error("Unsupported descriptor type");
            }
            vkBinding.descriptorCount = 1;
            bindings.push_back(vkBinding);
        }
        layoutInfo.pBindings = bindings.data();
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());

        if (vkCreateDescriptorSetLayout(m_device.vkDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create descriptor set layout");
        }

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        const auto descriptorPool = m_device.descriptorPool();
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_descriptorSetLayout;
        if (vkAllocateDescriptorSets(m_device.vkDevice(), &allocInfo, &m_descriptorSet) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate descriptor set");
        }
    }
    VulkanComputeRayTracingDescriptorSet::~VulkanComputeRayTracingDescriptorSet()
    {
        vkDestroyDescriptorSetLayout(m_device.vkDevice(), m_descriptorSetLayout, nullptr);
    }
    void VulkanComputeRayTracingDescriptorSet::setImage2D(uint32_t binding, Image2D *image)
    {
        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = binding;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writeDesc.descriptorCount = 1;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = static_cast<VulkanImage2D *>(image)->vkImageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writeDesc.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }
    void VulkanComputeRayTracingDescriptorSet::setBuffer(uint32_t binding, Buffer *buffer)
    {
        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = binding;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writeDesc.descriptorCount = 1;

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = static_cast<VulkanBuffer *>(buffer)->vkBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;
        writeDesc.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }
    void VulkanComputeRayTracingDescriptorSet::setAccelerationStructure(uint32_t binding, const TopLevelAccelerationStructure *tlas)
    {
        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = binding;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writeDesc.descriptorCount = 1;

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->instancesBuffer()->vkBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;
        writeDesc.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }
}