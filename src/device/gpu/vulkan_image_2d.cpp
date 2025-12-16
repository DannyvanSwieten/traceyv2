#include "vulkan_image_2d.hpp"
#include "vulkan_compute_device.hpp"
namespace tracey
{
    VulkanImage2D::VulkanImage2D(VulkanComputeDevice &device, uint32_t width, uint32_t height, ImageFormat format)
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        switch (format)
        {
        case ImageFormat::R8G8B8A8Unorm:
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case ImageFormat::R32G32B32A32Sfloat:
            imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            break;
        case ImageFormat::R32Sfloat:
            imageInfo.format = VK_FORMAT_R32_SFLOAT;
            break;
        default:
            throw std::runtime_error("Unsupported image format");
        }

        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device.vkDevice(), &imageInfo, nullptr, &m_image) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan image");
        }

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device.vkDevice(), m_image, &memRequirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device.findMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device.vkDevice(), &allocInfo, nullptr, &m_memory) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate Vulkan image memory");
        }

        if (vkBindImageMemory(device.vkDevice(), m_image, m_memory, 0) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to bind Vulkan image memory");
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device.vkDevice(), &viewInfo, nullptr, &m_imageView) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan image view");
        }
    }
    VulkanImage2D::~VulkanImage2D()
    {
        vkDestroyImageView(m_device, m_imageView, nullptr);
        vkDestroyImage(m_device, m_image, nullptr);
        vkFreeMemory(m_device, m_memory, nullptr);
    }
    char *VulkanImage2D::data()
    {
        return nullptr;
    }
}