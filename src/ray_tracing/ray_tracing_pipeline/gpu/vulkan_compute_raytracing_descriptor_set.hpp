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
        VulkanComputeRayTracingDescriptorSet(VulkanComputeDevice &device, VkDescriptorSetLayout descriptorSetLayout);
        ~VulkanComputeRayTracingDescriptorSet() override;

        void setImage2D(uint32_t binding, Image2D *image) override;
        void setBuffer(uint32_t binding, Buffer *buffer) override;
        void setAccelerationStructure(uint32_t binding, const TopLevelAccelerationStructure *tlas) override;

        VkDescriptorSet vkDescriptorSet() const { return m_descriptorSet; }

    private:
        VulkanComputeDevice &m_device;
        VkDescriptorSetLayout m_descriptorSetLayout;
        VkDescriptorSet m_descriptorSet;

        constexpr static uint32_t AccelerationStructureDescriptorCount = 6;
    };
}