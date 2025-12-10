#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <cstdint>
#include <span>

namespace tracey
{
    class VulkanContext
    {
    public:
        VulkanContext();
        ~VulkanContext();

        VulkanContext(const VulkanContext &) = delete;
        VulkanContext &operator=(const VulkanContext &) = delete;

        VkInstance instance() const { return m_instance; }
        VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
        VkDevice device() const { return m_device; }
        uint32_t computeQueueFamilyIndex() const { return m_computeQueueFamilyIndex; }
        VkQueue computeQueue() const { return m_computeQueue; }

        VkImage createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
        VkDeviceMemory allocateImage(VkImage image, VkMemoryPropertyFlags properties);
        VkImageView createImageView(VkImage image, VkFormat format);

        VkDescriptorPool createDescriptorPool(uint32_t maxSets, const std::span<const VkDescriptorPoolSize> poolSizes);

        VkShaderModule createShaderModule(const std::span<const uint32_t> code);
        VkPipeline createComputePipeline(VkShaderModule module, VkPipelineLayout pipelineLayout);

    private:
        void createInstance();
        void pickPhysicalDevice();
        void createDeviceAndQueue();
        void createCommandPool();

        VkCommandBuffer beginSingleTimeCommands();
        void endSingleTimeCommands(VkCommandBuffer cmd);

        VkInstance m_instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;

        uint32_t m_computeQueueFamilyIndex = 0;
        VkQueue m_computeQueue = VK_NULL_HANDLE;

        VkCommandPool m_commandPool = VK_NULL_HANDLE;
    };
} // namespace tracey