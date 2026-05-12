#include "vulkan_compute_raytracing_descriptor_set.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include "../../../device/gpu/vulkan_compute_device.hpp"
#include "../../../device/gpu/vulkan_image_2d.hpp"
#include "../../../device/gpu/vulkan_buffer.hpp"
#include "../../../device/gpu/vulkan_compute_top_level_acceleration_structure.hpp"
#include <stdexcept>
#include <vector>

namespace tracey
{
    VulkanComputeRayTracingDescriptorSet::VulkanComputeRayTracingDescriptorSet(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, VkDescriptorSetLayout descriptorSetLayout, uint32_t userBindingOffset) : m_device(device), m_descriptorSetLayout(descriptorSetLayout), m_descriptorSet(VK_NULL_HANDLE), m_layout(layout), m_userBindingOffset(userBindingOffset)
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
    void VulkanComputeRayTracingDescriptorSet::setImage2D(const std::string_view binding, Image2D *image)
    {
        const auto index = m_layout.indexForBinding(binding) + m_userBindingOffset;
        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = index;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writeDesc.descriptorCount = 1;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = static_cast<VulkanImage2D *>(image)->vkImageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writeDesc.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }
    void VulkanComputeRayTracingDescriptorSet::setBuffer(const std::string_view binding, Buffer *buffer)
    {
        const auto index = m_layout.indexForBinding(binding) + m_userBindingOffset;
        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = index;
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
    void VulkanComputeRayTracingDescriptorSet::setUniformBuffer(const std::string_view binding, Buffer *buffer)
    {
        const auto layoutIndex = m_layout.indexForBinding(binding);
        const auto index = layoutIndex + m_userBindingOffset;

        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = index;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writeDesc.descriptorCount = 1;

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = static_cast<VulkanBuffer *>(buffer)->vkBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;
        writeDesc.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }
    void VulkanComputeRayTracingDescriptorSet::setAccelerationStructure(const std::string_view binding, const TopLevelAccelerationStructure *tlas)
    {
        const auto index = static_cast<uint32_t>(m_layout.indexForBinding(binding));
        VkWriteDescriptorSet writeDesc[8]{
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = index + 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = index + 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = index + 2,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = index + 3,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = index + 4,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = index + 5,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = index + 6,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_descriptorSet,
                .dstBinding = index + 7,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
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

        VkDescriptorBufferInfo instanceInverseTransformsBufferInfo{};
        instanceInverseTransformsBufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->instanceInverseTransformsBuffer()->vkBuffer();
        instanceInverseTransformsBufferInfo.offset = 0;
        instanceInverseTransformsBufferInfo.range = VK_WHOLE_SIZE;
        writeDesc[5].pBufferInfo = &instanceInverseTransformsBufferInfo;

        VkDescriptorBufferInfo tlasNodesBufferInfo{};
        tlasNodesBufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->tlasNodesBuffer()->vkBuffer();
        tlasNodesBufferInfo.offset = 0;
        tlasNodesBufferInfo.range = VK_WHOLE_SIZE;
        writeDesc[6].pBufferInfo = &tlasNodesBufferInfo;

        VkDescriptorBufferInfo tlasInstanceIndicesBufferInfo{};
        tlasInstanceIndicesBufferInfo.buffer = static_cast<const VulkanComputeTopLevelAccelerationStructure *>(tlas)->tlasInstanceIndicesBuffer()->vkBuffer();
        tlasInstanceIndicesBufferInfo.offset = 0;
        tlasInstanceIndicesBufferInfo.range = VK_WHOLE_SIZE;
        writeDesc[7].pBufferInfo = &tlasInstanceIndicesBufferInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 8, writeDesc, 0, nullptr);
    }

    void VulkanComputeRayTracingDescriptorSet::setSampledTexture(uint32_t bindingIndex, Image2D *image)
    {
        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = bindingIndex;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeDesc.descriptorCount = 1;

        auto *vulkanImage = static_cast<VulkanImage2D *>(image);
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = vulkanImage->vkImageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.sampler = vulkanImage->vkSampler();
        writeDesc.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }

    void VulkanComputeRayTracingDescriptorSet::setSampledTextureArray(uint32_t bindingIndex, std::span<Image2D *> images)
    {
        if (images.empty())
            return;

        std::vector<VkDescriptorImageInfo> imageInfos(images.size());
        for (size_t i = 0; i < images.size(); ++i)
        {
            auto *vulkanImage = static_cast<VulkanImage2D *>(images[i]);
            imageInfos[i].imageView = vulkanImage->vkImageView();
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[i].sampler = vulkanImage->vkSampler();
        }

        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = bindingIndex;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeDesc.descriptorCount = static_cast<uint32_t>(images.size());
        writeDesc.pImageInfo = imageInfos.data();

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }

    void VulkanComputeRayTracingDescriptorSet::setSampledTextureArray(const std::string_view name, std::span<Image2D *> images)
    {
        const auto index = m_layout.indexForBinding(name) + m_userBindingOffset;
        setSampledTextureArray(static_cast<uint32_t>(index), images);
    }

    void VulkanComputeRayTracingDescriptorSet::setStorageBuffer(uint32_t bindingIndex, Buffer *buffer)
    {
        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = bindingIndex;
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

    void VulkanComputeRayTracingDescriptorSet::setSampler(const std::string_view name, SamplerKind kind)
    {
        const auto index = m_layout.indexForBinding(name) + m_userBindingOffset;

        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = index;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writeDesc.descriptorCount = 1;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = m_device.samplerForKind(kind);
        writeDesc.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }

    void VulkanComputeRayTracingDescriptorSet::setSampledImageArray(const std::string_view name, std::span<Image2D *> images)
    {
        const auto index = m_layout.indexForBinding(name) + m_userBindingOffset;

        std::vector<VkDescriptorImageInfo> imageInfos;
        imageInfos.reserve(images.size());

        for (const auto &img : images)
        {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageView = static_cast<VulkanImage2D *>(img)->vkImageView();
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos.push_back(imageInfo);
        }

        VkWriteDescriptorSet writeDesc{};
        writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDesc.dstSet = m_descriptorSet;
        writeDesc.dstBinding = index;
        writeDesc.dstArrayElement = 0;
        writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writeDesc.descriptorCount = static_cast<uint32_t>(imageInfos.size());
        writeDesc.pImageInfo = imageInfos.data();

        vkUpdateDescriptorSets(m_device.vkDevice(), 1, &writeDesc, 0, nullptr);
    }
}