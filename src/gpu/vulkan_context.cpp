#include "vulkan_context.hpp"
#include "vulkan_queue_sync.hpp"
#include <vector>
#include <stdexcept>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <volk.h>
#include <cstring>
#include <string>

namespace tracey
{
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
        void *pUserData)
    {
        (void)messageType;
        (void)pUserData;

        std::string severity;
        switch (messageSeverity)
        {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            severity = "VERBOSE";
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            severity = "INFO";
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            severity = "WARNING";
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            severity = "ERROR";
            break;
        default:
            severity = "UNKNOWN";
            break;
        }

        std::cerr << "[Vulkan] " << severity << ": " << pCallbackData->pMessage << std::endl;

        return VK_FALSE;
    }

    uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeFilter, VkMemoryPropertyFlags props)
    {
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((typeFilter & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & props) == props)
            {
                return i;
            }
        }
        throw std::runtime_error("Failed to find suitable memory type");
    }

    static bool hasLayer(const char *name)
    {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        for (const auto &l : layers)
        {
            if (std::strcmp(l.layerName, name) == 0)
                return true;
        }
        return false;
    }

    static bool hasInstanceExtension(const char *name)
    {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
        for (const auto &e : exts)
        {
            if (std::strcmp(e.extensionName, name) == 0)
                return true;
        }
        return false;
    }

    static VkResult createDebugMessenger(VkInstance instance,
                                         const VkDebugUtilsMessengerCreateInfoEXT *createInfo,
                                         VkDebugUtilsMessengerEXT *messenger)
    {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (!fn)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        return fn(instance, createInfo, nullptr, messenger);
    }

    static void destroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger)
    {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn && messenger != VK_NULL_HANDLE)
            fn(instance, messenger, nullptr);
    }

    VulkanContext::VulkanContext() : VulkanContext(VulkanContextConfig{}) {}

    VulkanContext::VulkanContext(VulkanContextConfig config) : m_config(config)
    {
#ifndef NDEBUG
        m_enableValidation = true;
#else
        m_enableValidation = false;
#endif
        VkResult res = volkInitialize();
        if (res != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to initialize Volk");
        }
        createInstance();
        pickPhysicalDevice();
        createDeviceAndQueue();
    }

    VulkanContext::~VulkanContext()
    {
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
            vkDestroyDevice(m_device, nullptr);
        }

        if (m_instance != VK_NULL_HANDLE)
        {
            destroyDebugMessenger(m_instance, m_debugMessenger);
            m_debugMessenger = VK_NULL_HANDLE;
            vkDestroyInstance(m_instance, nullptr);
        }
    }

    VulkanContext::VulkanContext(VulkanContext &&other) : m_config(other.m_config),
                                                          m_instance(other.m_instance),
                                                          m_physicalDevice(other.m_physicalDevice),
                                                          m_device(other.m_device),
                                                          m_computeQueueFamilyIndex(other.m_computeQueueFamilyIndex),
                                                          m_computeQueue(other.m_computeQueue),
                                                          m_commandPool(other.m_commandPool),
                                                          m_enableValidation(other.m_enableValidation),
                                                          m_hasMetalObjects(other.m_hasMetalObjects),
                                                          m_debugMessenger(other.m_debugMessenger)
    {
        other.m_instance = VK_NULL_HANDLE;
        other.m_physicalDevice = VK_NULL_HANDLE;
        other.m_device = VK_NULL_HANDLE;
        other.m_computeQueue = VK_NULL_HANDLE;
        other.m_commandPool = VK_NULL_HANDLE;
        other.m_debugMessenger = VK_NULL_HANDLE;
    }

    void VulkanContext::createInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Raytracer";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "Custom";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        std::vector<const char *> extensions;
        std::vector<const char *> layers;

        // macOS / MoltenVK: portability enumeration
        if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        }

        // Debug utils for validation output
        if (m_enableValidation && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        // Surface extensions for windowed presentation. Only enabled when the
        // caller asked for a present-capable context. Extension name macros
        // for platform-specific surfaces aren't defined by volk by default
        // (they require VK_USE_PLATFORM_* before include), so use the stable
        // string names directly.
        if (m_config.enablePresentation)
        {
            if (hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME))
                extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef __APPLE__
            if (hasInstanceExtension("VK_EXT_metal_surface"))
                extensions.push_back("VK_EXT_metal_surface");
#endif
#ifdef _WIN32
            if (hasInstanceExtension("VK_KHR_win32_surface"))
                extensions.push_back("VK_KHR_win32_surface");
#endif
        }

        // Validation layer (only if present)
        if (m_enableValidation && hasLayer("VK_LAYER_KHRONOS_validation"))
        {
            layers.push_back("VK_LAYER_KHRONOS_validation");
        }

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();

        // Required on Apple platforms (MoltenVK) when using portability enumeration.
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (m_enableValidation && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugCreateInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugCreateInfo.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugCreateInfo.pfnUserCallback = debugCallback;
            createInfo.pNext = &debugCreateInfo;
        }
        else
        {
            createInfo.pNext = nullptr;
        }

        VkResult res = vkCreateInstance(&createInfo, nullptr, &m_instance);
        if (res != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan instance (vkCreateInstance)");
        }

        volkLoadInstance(m_instance);

        if (m_enableValidation && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            VkDebugUtilsMessengerCreateInfoEXT dbg{};
            dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dbg.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbg.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg.pfnUserCallback = debugCallback;

            (void)createDebugMessenger(m_instance, &dbg, &m_debugMessenger);
        }
    }

    void VulkanContext::pickPhysicalDevice()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
        if (deviceCount == 0)
        {
            throw std::runtime_error("Failed: no Vulkan-capable GPUs found");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

        // For now, pick the first device with a compute-capable queue family.
        for (auto dev : devices)
        {
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);

            std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, families.data());

            for (uint32_t i = 0; i < queueFamilyCount; ++i)
            {
                if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
                {
                    m_physicalDevice = dev;
                    m_computeQueueFamilyIndex = i;
                    return;
                }
            }
        }

        throw std::runtime_error("Failed: no GPU with compute queue found");
    }

    void VulkanContext::createDeviceAndQueue()
    {
        float queuePriority = 1.0f;

        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = m_computeQueueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceFeatures supportedFeatures{};
        vkGetPhysicalDeviceFeatures(m_physicalDevice, &supportedFeatures);

        VkPhysicalDeviceFeatures deviceFeatures{};
        // Wireframe rendering uses VK_POLYGON_MODE_LINE, which requires this.
        if (supportedFeatures.fillModeNonSolid)
        {
            deviceFeatures.fillModeNonSolid = VK_TRUE;
        }

        // Enable descriptor indexing for bindless textures (Vulkan 1.2 core)
        // On macOS, requires MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1 environment variable
        VkPhysicalDeviceDescriptorIndexingFeatures indexingFeatures{};
        indexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        indexingFeatures.pNext = nullptr;
        indexingFeatures.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        indexingFeatures.descriptorBindingVariableDescriptorCount = VK_TRUE;
        indexingFeatures.runtimeDescriptorArray = VK_TRUE;
        indexingFeatures.descriptorBindingPartiallyBound = VK_TRUE;
        indexingFeatures.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;  // Required for UPDATE_AFTER_BIND

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &indexingFeatures;  // Chain descriptor indexing features
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.pEnabledFeatures = &deviceFeatures;

        std::vector<const char *> devExts;

#ifdef __APPLE__
        // MoltenVK exposes VK_KHR_portability_subset; some apps need to enable it explicitly.
        devExts.push_back("VK_KHR_portability_subset");

        // VK_EXT_metal_objects (probed — older MoltenVK builds lack it):
        // lets us export a VkImage's backing MTLTexture so the Metal path
        // tracer backend can render straight into the image the viewport
        // compositor blits. Zero-copy Metal↔Vulkan output sharing.
        {
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> available(extCount);
            vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, available.data());
            for (const auto &ext : available)
            {
                if (std::strcmp(ext.extensionName, "VK_EXT_metal_objects") == 0)
                {
                    devExts.push_back("VK_EXT_metal_objects");
                    m_hasMetalObjects = true;
                    break;
                }
            }
        }
#endif

        if (m_config.enablePresentation)
        {
            devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }

        createInfo.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
        createInfo.ppEnabledExtensionNames = devExts.empty() ? nullptr : devExts.data();

        if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create logical device");
        }

        volkLoadDevice(m_device);

        vkGetDeviceQueue(m_device, m_computeQueueFamilyIndex, 0, &m_computeQueue);
    }

    VkCommandBuffer VulkanContext::beginSingleTimeCommands()
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = m_commandPool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;

        VkCommandBuffer cmd;
        if (vkAllocateCommandBuffers(m_device, &alloc, &cmd) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate command buffer");
        }

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(cmd, &begin);
        return cmd;
    }

    void VulkanContext::endSingleTimeCommands(VkCommandBuffer cmd)
    {
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        VkFence fence;
        vkCreateFence(m_device, &fenceInfo, nullptr, &fence);

        {
            std::lock_guard<std::mutex> queueLock(vulkanQueueMutex());
            vkQueueSubmit(m_computeQueue, 1, &submit, fence);
        }
        vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);

        vkDestroyFence(m_device, fence, nullptr);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    }
}