#pragma once
#include "device/device.hpp"
#include "../../gpu/vulkan_context.hpp"
namespace tracey
{
    class VulkanComputeDevice : public Device
    {
    public:
        VulkanComputeDevice();
        ~VulkanComputeDevice() override;

        VkDevice vkDevice() const;

        RayTracingPipeline *createRayTracingPipeline(const RayTracingPipelineLayout &layout, const ShaderBindingTable *sbt) override;
        ShaderModule *createShaderModule(ShaderStage stage, const std::string_view source, const std::string_view entryPoint) override;
        ShaderBindingTable *createShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders) override;
        RayTracingCommandBuffer *createRayTracingCommandBuffer() override;
        void allocateDescriptorSets(std::span<DescriptorSet *> sets, const RayTracingPipelineLayout &layout) override;
        Buffer *createBuffer(uint32_t size, BufferUsage usageFlags) override;
        BottomLevelAccelerationStructure *createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount) override;

    private:
        VulkanContext m_vulkanContext;
    };
}