#pragma once

#include <vulkan/vulkan.h>
#include "../image_2d.hpp"
#include "../device.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class VulkanImage2D : public Image2D
    {
    public:
        VulkanImage2D(VulkanComputeDevice &device, uint32_t width, uint32_t height, ImageFormat format);
        ~VulkanImage2D();

        char *data() override;

        VkImage vkImage() const { return m_image; }
        VkDeviceMemory vkDeviceMemory() const { return m_memory; }
        VkImageView vkImageView() const { return m_imageView; }

    private:
        VkDevice m_device;
        VkImage m_image;
        VkDeviceMemory m_memory;
        VkImageView m_imageView;
    };
} // namespace tracey