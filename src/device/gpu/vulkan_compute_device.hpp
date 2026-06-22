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

        Buffer *createBuffer(uint32_t size, BufferUsage usageFlags) override;
        Image2D *createImage2D(uint32_t width, uint32_t height, ImageFormat format) override;
        Image2D *createImage2DWithData(uint32_t width, uint32_t height, ImageFormat format,
                                       const void *data, size_t dataSize,
                                       SamplerFilter filter = SamplerFilter::Linear,
                                       SamplerAddressMode addressMode = SamplerAddressMode::Repeat) override;
        BottomLevelAccelerationStructure *createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount, const BVHConfig &bvhConfig = {}) override;
        TopLevelAccelerationStructure *createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances) override;
        uint32_t maxBindlessTextures() const override;
        void waitIdle() override;

        int findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
        uint32_t queueFamilyIndex() const { return m_vulkanContext.computeQueueFamilyIndex(); }

        VkDescriptorPool descriptorPool() const { return m_descriptorPool; }
        VkCommandPool commandPool() const { return m_commandPool; }
        VkQueue computeQueue() const { return m_vulkanContext.computeQueue(); }

        // Direct access to the underlying VulkanContext, needed by the editor
        // to construct a VulkanPresenter against the same instance/device.
        VulkanContext &context() { return m_vulkanContext; }
        const VulkanContext &context() const { return m_vulkanContext; }

        // Get fixed samplers for bindless texture support. The renderer
        // always keeps these four combos around so per-texture sampler
        // choice (driven by glTF wrap modes and the linear-vs-color
        // classification) is a 2-bit index, not a descriptor rebind.
        VkSampler linearRepeatSampler() const { return m_linearSampler; }
        VkSampler linearClampSampler() const { return m_linearClampSampler; }
        VkSampler nearestRepeatSampler() const { return m_nearestSampler; }
        VkSampler nearestClampSampler() const { return m_nearestClampSampler; }
        // Back-compat shims for callers still on the old two-sampler API.
        VkSampler linearSampler() const { return m_linearSampler; }
        VkSampler nearestSampler() const { return m_nearestSampler; }
        VkSampler samplerForKind(SamplerKind kind) const;

    private:
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        VkSampler m_linearSampler = VK_NULL_HANDLE;
        VkSampler m_linearClampSampler = VK_NULL_HANDLE;
        VkSampler m_nearestSampler = VK_NULL_HANDLE;
        VkSampler m_nearestClampSampler = VK_NULL_HANDLE;

    private:
        VulkanContext m_vulkanContext;
    };
}