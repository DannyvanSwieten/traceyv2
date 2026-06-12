#pragma once

#include <volk.h>
#include "../image_2d.hpp"
#include "../device.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class VulkanImage2D : public Image2D
    {
    public:
        VulkanImage2D(VulkanComputeDevice &device, uint32_t width, uint32_t height, ImageFormat format);
        // Storage image whose backing MTLTexture is exportable through
        // VK_EXT_metal_objects (pass exportMetalTexture=true). The Metal
        // path tracer backend renders into the exported texture while the
        // Vulkan compositor blits this very image — zero-copy sharing.
        // Requires the device to have been created with VK_EXT_metal_objects
        // (VulkanContext::hasMetalObjectsExtension()); throws otherwise.
        VulkanImage2D(VulkanComputeDevice &device, uint32_t width, uint32_t height, ImageFormat format,
                      bool exportMetalTexture);
        VulkanImage2D(VulkanComputeDevice &device, uint32_t width, uint32_t height, ImageFormat format,
                      const void *data, size_t dataSize,
                      SamplerFilter filter = SamplerFilter::Linear,
                      SamplerAddressMode addressMode = SamplerAddressMode::Repeat);
        ~VulkanImage2D();

        VulkanImage2D(const VulkanImage2D &) = delete;
        VulkanImage2D &operator=(const VulkanImage2D &) = delete;

        char *data() override;

        // Re-upload pixel data into an existing image. Synchronous (blocks
        // until upload completes). Leaves the image in `finalLayout` for the
        // next consumer (default: VK_IMAGE_LAYOUT_GENERAL, matching what
        // VulkanPresenter expects from a blit source).
        void uploadPixels(VulkanComputeDevice &device, const void *data, size_t dataSize,
                          VkImageLayout finalLayout = VK_IMAGE_LAYOUT_GENERAL);

        VkImage vkImage() const { return m_image; }
        VkDeviceMemory vkDeviceMemory() const { return m_memory; }
        VkImageView vkImageView() const { return m_imageView; }
        VkSampler vkSampler() const { return m_sampler; }
        bool hasSampler() const { return m_sampler != VK_NULL_HANDLE; }

        // The image's backing id<MTLTexture> (returned as an opaque pointer
        // so this header stays ObjC-free), fetched via
        // vkExportMetalObjectsEXT. Only valid for images constructed with
        // exportMetalTexture=true; returns nullptr otherwise or off-Apple.
        void *exportedMetalTexture() const { return m_exportedMetalTexture; }

        uint32_t width() const override { return m_width; }
        uint32_t height() const override { return m_height; }

    private:
        void createImage(VulkanComputeDevice &device, ImageFormat format, bool forTexture,
                         bool exportMetalTexture = false);
        void createSampler(VulkanComputeDevice &device, SamplerFilter filter, SamplerAddressMode addressMode);
        void uploadData(VulkanComputeDevice &device, const void *data, size_t dataSize);

        VkDevice m_device;
        VkImage m_image = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        VkImageView m_imageView = VK_NULL_HANDLE;
        VkSampler m_sampler = VK_NULL_HANDLE;
        VkFormat m_vkFormat = VK_FORMAT_UNDEFINED;

        uint32_t m_width;
        uint32_t m_height;
        void *m_exportedMetalTexture = nullptr;  // id<MTLTexture>, unretained
    };
} // namespace tracey