#pragma once
#include <volk.h>
#include "../descriptor_set.hpp"
namespace tracey
{
    class VulkanComputeDevice;
    class RayTracingPipelineLayoutDescriptor;
    class VulkanComputeRayTracingDescriptorSet : public DescriptorSet
    {
    public:
        VulkanComputeRayTracingDescriptorSet(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, VkDescriptorSetLayout descriptorSetLayout, uint32_t userBindingOffset = 5);
        ~VulkanComputeRayTracingDescriptorSet() override;

        void setImage2D(const std::string_view binding, Image2D *image) override;
        void setBuffer(const std::string_view binding, Buffer *buffer) override;
        void setAccelerationStructure(const std::string_view binding, const TopLevelAccelerationStructure *tlas) override;

        VkDescriptorSet vkDescriptorSet() const { return m_descriptorSet; }

    private:
        VulkanComputeDevice &m_device;
        VkDescriptorSetLayout m_descriptorSetLayout;
        VkDescriptorSet m_descriptorSet;
        const RayTracingPipelineLayoutDescriptor &m_layout;
        uint32_t m_userBindingOffset;
    };
}