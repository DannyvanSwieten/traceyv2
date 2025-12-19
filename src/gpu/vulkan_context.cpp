#include "vulkan_context.hpp"
#include <vector>
#include <stdexcept>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <volk.h>

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

    VulkanContext::VulkanContext()
    {
        VkResult res = volkInitialize();
        if (res != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to initialize Volk");
        }
        createInstance();
        pickPhysicalDevice();
        createDeviceAndQueue();
        createCommandPool();
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
            vkDestroyInstance(m_instance, nullptr);
        }
    }

    VulkanContext::VulkanContext(VulkanContext &&other) : m_instance(other.m_instance),
                                                          m_physicalDevice(other.m_physicalDevice),
                                                          m_device(other.m_device),
                                                          m_computeQueueFamilyIndex(other.m_computeQueueFamilyIndex),
                                                          m_computeQueue(other.m_computeQueue),
                                                          m_commandPool(other.m_commandPool)
    {
        other.m_instance = VK_NULL_HANDLE;
        other.m_physicalDevice = VK_NULL_HANDLE;
        other.m_device = VK_NULL_HANDLE;
        other.m_computeQueue = VK_NULL_HANDLE;
        other.m_commandPool = VK_NULL_HANDLE;
    }

    void VulkanContext::createInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Raytracer";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "Custom";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_4;

        // --- macOS / MoltenVK: portability extension + flag ---

        const char *extensions[] = {
            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, // "VK_KHR_portability_enumeration"
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME              // "VK_EXT_debug_utils"
        };

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
        createInfo.ppEnabledExtensionNames = extensions;

        // Required on Apple platforms:
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

        // No layers to keep it minimal for now
        createInfo.enabledLayerCount = 0;
        createInfo.ppEnabledLayerNames = nullptr;

        // Add debug messenger info
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;
        createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT *)&debugCreateInfo;

        VkResult res = vkCreateInstance(&createInfo, nullptr, &m_instance);
        if (res != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan instance (vkCreateInstance)");
        }

        volkLoadInstance(m_instance);
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

        VkPhysicalDeviceFeatures deviceFeatures{};
        // Enable features if you need them later (e.g. shaderInt64, etc.)

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.pEnabledFeatures = &deviceFeatures;

        // No device extensions yet; we only need compute.
        createInfo.enabledExtensionCount = 0;
        createInfo.ppEnabledExtensionNames = nullptr;

        if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create logical device");
        }

        volkLoadDevice(m_device);

        vkGetDeviceQueue(m_device, m_computeQueueFamilyIndex, 0, &m_computeQueue);
    }

    void VulkanContext::createCommandPool()
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = m_computeQueueFamilyIndex;

        if (vkCreateCommandPool(m_device, &info, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create command pool");
        }
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

        vkQueueSubmit(m_computeQueue, 1, &submit, fence);
        vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);

        vkDestroyFence(m_device, fence, nullptr);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    }

    VkPipeline VulkanContext::createComputePipeline(VkShaderModule module, VkPipelineLayout pipelineLayout)
    {
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";

        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage = stage;
        info.layout = pipelineLayout;

        VkPipeline pipeline;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        {
            vkDestroyShaderModule(m_device, module, nullptr);
            throw std::runtime_error("Failed to create compute pipeline");
        }

        vkDestroyShaderModule(m_device, module, nullptr);
    }

    VkImage VulkanContext::createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage)
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent.width = width;
        info.extent.height = height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.format = format;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = usage;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkImage image;
        if (vkCreateImage(m_device, &info, nullptr, &image) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create image");
        }
        return image;
    }

    VkDeviceMemory VulkanContext::allocateImage(VkImage image, VkMemoryPropertyFlags properties)
    {
        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_device, image, &memReq);

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = memReq.size;
        alloc.memoryTypeIndex = findMemoryType(
            m_physicalDevice,
            memReq.memoryTypeBits,
            properties);

        VkDeviceMemory memory;
        if (vkAllocateMemory(m_device, &alloc, nullptr, &memory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate image memory");
        }
        return memory;
    }

    VkImageView VulkanContext::createImageView(VkImage image, VkFormat format)
    {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;

        VkImageView imageView;
        if (vkCreateImageView(m_device, &view, nullptr, &imageView) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create image view");
        }
        return imageView;
    }

    VkDescriptorPool VulkanContext::createDescriptorPool(uint32_t maxSets, const std::span<const VkDescriptorPoolSize> poolSizes)
    {
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = maxSets;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        VkDescriptorPool descriptorPool;
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create descriptor pool");
        }
        return descriptorPool;
    }

    VkShaderModule VulkanContext::createShaderModule(const std::span<const uint32_t> code)
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();

        VkShaderModule module;
        if (vkCreateShaderModule(m_device, &info, nullptr, &module) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create shader module");
        }
        return module;
    }
}