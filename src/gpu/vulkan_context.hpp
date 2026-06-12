#pragma once

#include <volk.h>
#include <vector>
#include <optional>
#include <cstdint>
#include <span>

namespace tracey
{
    /// Options for constructing a VulkanContext.
    /// Default = compute-only context (existing behaviour). Set enablePresentation
    /// to true to add VK_KHR_surface + the platform surface extension + the
    /// VK_KHR_swapchain device extension, so the resulting context can drive a
    /// VulkanPresenter against a windowed surface.
    struct VulkanContextConfig
    {
        bool enablePresentation = false;
    };

    class VulkanContext
    {
    public:
        VulkanContext();
        explicit VulkanContext(VulkanContextConfig config);
        ~VulkanContext();

        VulkanContext(const VulkanContext &) = delete;
        VulkanContext &operator=(const VulkanContext &) = delete;

        VulkanContext(VulkanContext &&);

        VkInstance instance() const { return m_instance; }
        VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
        VkDevice device() const { return m_device; }
        uint32_t computeQueueFamilyIndex() const { return m_computeQueueFamilyIndex; }
        VkQueue computeQueue() const { return m_computeQueue; }
        // The current context picks a single compute queue and uses it for everything,
        // including graphics/present submissions. These aliases let consumers that
        // expect a separate graphics queue compile cleanly.
        uint32_t graphicsQueueFamilyIndex() const { return m_computeQueueFamilyIndex; }
        VkQueue graphicsQueue() const { return m_computeQueue; }
        const VulkanContextConfig &config() const { return m_config; }

        // True when the device was created with VK_EXT_metal_objects
        // (MoltenVK only). Gate for exporting a VkImage's backing MTLTexture
        // to the Metal path tracer backend.
        bool hasMetalObjectsExtension() const { return m_hasMetalObjects; }

    private:
        void createInstance();
        void pickPhysicalDevice();
        void createDeviceAndQueue();

        VkCommandBuffer beginSingleTimeCommands();
        void endSingleTimeCommands(VkCommandBuffer cmd);

        VulkanContextConfig m_config{};

        VkInstance m_instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;

        uint32_t m_computeQueueFamilyIndex = 0;
        VkQueue m_computeQueue = VK_NULL_HANDLE;

        VkCommandPool m_commandPool = VK_NULL_HANDLE;

        bool m_enableValidation = false;
        bool m_hasMetalObjects = false;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    };
} // namespace tracey
