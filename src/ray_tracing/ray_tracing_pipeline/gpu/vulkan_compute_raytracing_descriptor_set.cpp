#include "vulkan_compute_raytracing_descriptor_set.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include "../../../device/gpu/vulkan_compute_device.hpp"
#include "../../../device/gpu/vulkan_image_2d.hpp"
#include "../../../device/gpu/vulkan_buffer.hpp"
#include "../../../device/gpu/vulkan_compute_top_level_acceleration_structure.hpp"
#include <stdexcept>

namespace tracey
{
    VulkanComputeRayTracingDescriptorSet::VulkanComputeRayTracingDescriptorSet(VulkanComputeDevice &device, VkDescriptorSetLayout descriptorSetLayout) : m_device(device), m_descriptorSetLayout(descriptorSetLayout)
    {
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
    }
    void VulkanComputeRayTracingDescriptorSet::setImage2D(uint32_t binding, Image2D *image)
    {
        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = binding + AccelerationStructureDescriptorCount;
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
        writeDesc.dstBinding = binding + AccelerationStructureDescriptorCount;
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
        VkWriteDescriptorSet writeDesc[5]{
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = binding + 0,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = binding + 1,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = binding + 2,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = binding + 3,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = binding + 4,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
            },
        };

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->instancesBuffer()->vkBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;
        writeDesc[0].pBufferInfo = &bufferInfo;

        VkDescriptorBufferInfo asBufferInfo{};
        asBufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->asBuffer()->vkBuffer();
        asBufferInfo.offset = 0;
        asBufferInfo.range = VK_WHOLE_SIZE;
        writeDesc[1].pBufferInfo = &asBufferInfo;

        VkDescriptorBufferInfo blasBufferInfo{};
        blasBufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->blasInfoBuffer()->vkBuffer();
        blasBufferInfo.offset = 0;
        blasBufferInfo.range = VK_WHOLE_SIZE;
        writeDesc[2].pBufferInfo = &blasBufferInfo;

        VkDescriptorBufferInfo triangleInfoBufferInfo{};
        triangleInfoBufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->triangleInfoBuffer()->vkBuffer();
        triangleInfoBufferInfo.offset = 0;
        triangleInfoBufferInfo.range = VK_WHOLE_SIZE;
        writeDesc[3].pBufferInfo = &triangleInfoBufferInfo;

        VkDescriptorBufferInfo primitiveIndicesBufferInfo{};
        primitiveIndicesBufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->primitiveIndicesBuffer()->vkBuffer();
        primitiveIndicesBufferInfo.offset = 0;
        primitiveIndicesBufferInfo.range = VK_WHOLE_SIZE;
        writeDesc[4].pBufferInfo = &primitiveIndicesBufferInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 5, writeDesc, 0, nullptr);
    }
}