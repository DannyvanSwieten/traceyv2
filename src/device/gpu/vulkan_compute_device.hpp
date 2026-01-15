#pragma once
#include "device/device.hpp"
#include "../../gpu/vulkan_context.hpp"

namespace tracey
{
    class VulkanComputeDevice : public Device
    {
    public:
        VulkanComputeDevice(VulkanContext context);
        ~VulkanComputeDevice() override;

        VkDevice vkDevice() const;

        RayTracingPipeline *createRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const ShaderBindingTable *sbt) override;
        RayTracingPipeline *createWaveFrontRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const ShaderBindingTable *sbt) override;
        ShaderModule *createShaderModule(ShaderStage stage, const std::string_view source, const std::string_view entryPoint) override;
        ShaderBindingTable *createShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders, const std::span<const ShaderModule *> missShaders, const ShaderModule *resolveShader = nullptr) override;
        RayTracingCommandBuffer *createRayTracingCommandBuffer() override;
        Buffer *createBuffer(uint32_t size, BufferUsage usageFlags) override;
        Image2D *createImage2D(uint32_t width, uint32_t height, ImageFormat format) override;
        BottomLevelAccelerationStructure *createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount) override;
        TopLevelAccelerationStructure *createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances) override;

        int findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
        uint32_t queueFamilyIndex() const { return m_vulkanContext.computeQueueFamilyIndex(); }

        VkDescriptorPool descriptorPool() const { return m_descriptorPool; }
        VkCommandPool commandPool() const { return m_commandPool; }
        VkQueue computeQueue() const { return m_vulkanContext.computeQueue(); }

    private:
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;

    private:
        VulkanContext m_vulkanContext;
    };
}