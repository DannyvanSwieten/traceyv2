#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <cstdint>

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

    private:
        void createInstance();
        void pickPhysicalDevice();
        void createDeviceAndQueue();

        VkInstance m_instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;

        uint32_t m_computeQueueFamilyIndex = 0;
        VkQueue m_computeQueue = VK_NULL_HANDLE;
    };
} // namespace tracey