#pragma once
#include <volk.h>
#include "../descriptor_set.hpp"
namespace tracey
{
    class VulkanComputeDevice;
    class RayTracingPipelineLayout;
    class VulkanComputeRayTracingDescriptorSet : public DescriptorSet
    {
    public:
        VulkanComputeRayTracingDescriptorSet(VulkanComputeDevice &device, const RayTracingPipelineLayout &layout);
        ~VulkanComputeRayTracingDescriptorSet() override;

        void setImage2D(uint32_t binding, Image2D *image) override;
        void setBuffer(uint32_t binding, Buffer *buffer) override;
        void setAccelerationStructure(uint32_t binding, const TopLevelAccelerationStructure *tlas) override;

    private:
        VulkanComputeDevice &m_device;
        VkDescriptorSetLayout m_descriptorSetLayout;
        VkDescriptorSet m_descriptorSet;
    };
}