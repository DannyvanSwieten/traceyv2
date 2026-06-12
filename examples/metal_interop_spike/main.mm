// Metal ↔ Vulkan interop spike for the Metal RT path tracer backend.
//
// Proves the zero-copy output path end to end:
//   1. Create a normal tracey Vulkan device (MoltenVK) with
//      VK_EXT_metal_objects enabled.
//   2. Create a VulkanImage2D with exportMetalTexture=true and pull out the
//      backing id<MTLTexture> MoltenVK created for it.
//   3. Fill that texture from the METAL side (compute kernel writing a known
//      colour), wait for completion.
//   4. Read the image back through the VULKAN side (vkCmdCopyImageToBuffer)
//      and verify the pixels match what Metal wrote.
//
// If this passes, the Metal path tracer backend can render straight into the
// image the viewport compositor already blits — no CPU copies, no IOSurface
// plumbing.

#import <Metal/Metal.h>

#include "device/device.hpp"
#include "device/gpu/vulkan_compute_device.hpp"
#include "device/gpu/vulkan_image_2d.hpp"
#include "gpu/vulkan_context.hpp"

#include <volk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
    constexpr uint32_t kSize = 64;
    // What the Metal kernel writes; what Vulkan must read back.
    constexpr uint8_t kExpected[4] = {255, 128, 64, 255};

    const char *kFillKernel = R"(
#include <metal_stdlib>
using namespace metal;
kernel void fill(texture2d<float, access::write> out [[texture(0)]],
                 uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    out.write(float4(1.0, 0.50196, 0.25098, 1.0), gid);
}
)";

    bool fillFromMetal(void *mtlTexturePtr)
    {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)mtlTexturePtr;
        id<MTLDevice> device = texture.device;
        if (!device)
        {
            std::fprintf(stderr, "FAIL: exported MTLTexture has no device\n");
            return false;
        }
        std::printf("Metal device: %s\n", device.name.UTF8String);
        std::printf("MTLTexture: %lux%lu pixelFormat=%lu usage=%lu\n",
                    (unsigned long)texture.width, (unsigned long)texture.height,
                    (unsigned long)texture.pixelFormat, (unsigned long)texture.usage);

        NSError *error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(kFillKernel)
                                                  options:nil
                                                    error:&error];
        if (!lib)
        {
            std::fprintf(stderr, "FAIL: MSL compile: %s\n",
                         error.localizedDescription.UTF8String);
            return false;
        }
        id<MTLFunction> fn = [lib newFunctionWithName:@"fill"];
        id<MTLComputePipelineState> pso =
            [device newComputePipelineStateWithFunction:fn error:&error];
        if (!pso)
        {
            std::fprintf(stderr, "FAIL: pipeline: %s\n",
                         error.localizedDescription.UTF8String);
            return false;
        }

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setTexture:texture atIndex:0];
        MTLSize tg = MTLSizeMake(8, 8, 1);
        MTLSize grid = MTLSizeMake((kSize + 7) / 8, (kSize + 7) / 8, 1);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        if (cmd.status == MTLCommandBufferStatusError)
        {
            std::fprintf(stderr, "FAIL: Metal command buffer error: %s\n",
                         cmd.error.localizedDescription.UTF8String);
            return false;
        }
        return true;
    }

    bool readbackViaVulkan(tracey::VulkanComputeDevice &device,
                           tracey::VulkanImage2D &image,
                           std::vector<uint8_t> &outPixels)
    {
        VkDevice vk = device.vkDevice();
        const VkDeviceSize byteSize = kSize * kSize * 4;

        // Host-visible staging buffer.
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = byteSize;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vk, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements reqs;
        vkGetBufferMemoryRequirements(vk, buffer, &reqs);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = reqs.size;
        ai.memoryTypeIndex = device.findMemoryType(
            reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(vk, &ai, nullptr, &memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(vk, buffer, memory, 0);

        // One-shot copy: the image sits in GENERAL layout (set at creation);
        // a memory barrier makes the Metal writes visible to the transfer.
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandPool = device.commandPool();
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        if (vkAllocateCommandBuffers(vk, &cai, &cmd) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbi);

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.vkImage();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kSize, kSize, 1};
        vkCmdCopyImageToBuffer(cmd, image.vkImage(), VK_IMAGE_LAYOUT_GENERAL,
                               buffer, 1, &region);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VkQueue queue = device.context().computeQueue();
        if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
        vkQueueWaitIdle(queue);

        void *mapped = nullptr;
        vkMapMemory(vk, memory, 0, byteSize, 0, &mapped);
        outPixels.resize(byteSize);
        std::memcpy(outPixels.data(), mapped, byteSize);
        vkUnmapMemory(vk, memory);

        vkFreeCommandBuffers(vk, device.commandPool(), 1, &cmd);
        vkDestroyBuffer(vk, buffer, nullptr);
        vkFreeMemory(vk, memory, nullptr);
        return true;
    }
}

int main()
{
    @autoreleasepool
    {
        std::unique_ptr<tracey::Device> device(
            tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute));
        auto *vkDevice = dynamic_cast<tracey::VulkanComputeDevice *>(device.get());
        if (!vkDevice)
        {
            std::fprintf(stderr, "FAIL: no Vulkan compute device\n");
            return 1;
        }
        if (!vkDevice->context().hasMetalObjectsExtension())
        {
            std::fprintf(stderr, "FAIL: VK_EXT_metal_objects not available\n");
            return 1;
        }
        std::printf("VK_EXT_metal_objects: enabled\n");

        tracey::VulkanImage2D image(*vkDevice, kSize, kSize,
                                    tracey::ImageFormat::R8G8B8A8Unorm,
                                    /*exportMetalTexture=*/true);
        if (!image.exportedMetalTexture())
        {
            std::fprintf(stderr, "FAIL: no exported MTLTexture\n");
            return 1;
        }
        std::printf("Exported MTLTexture: %p\n", image.exportedMetalTexture());

        if (!fillFromMetal(image.exportedMetalTexture())) return 1;

        std::vector<uint8_t> pixels;
        if (!readbackViaVulkan(*vkDevice, image, pixels))
        {
            std::fprintf(stderr, "FAIL: Vulkan readback failed\n");
            return 1;
        }

        // Verify a few sample points (corners + centre) with ±1 tolerance
        // for unorm rounding.
        const size_t samples[] = {
            0,
            (kSize - 1) * 4u,
            (static_cast<size_t>(kSize) * (kSize - 1)) * 4u,
            (static_cast<size_t>(kSize) * kSize - 1) * 4u,
            (static_cast<size_t>(kSize) * (kSize / 2) + kSize / 2) * 4u,
        };
        for (size_t off : samples)
        {
            for (int c = 0; c < 4; ++c)
            {
                const int got = pixels[off + c];
                const int want = kExpected[c];
                if (std::abs(got - want) > 1)
                {
                    std::fprintf(stderr,
                                 "FAIL: pixel@%zu channel %d: got %d want %d\n",
                                 off / 4, c, got, want);
                    return 1;
                }
            }
        }

        std::printf("PASS: Metal wrote, Vulkan read back (%ux%u, RGBA8)\n", kSize, kSize);
        return 0;
    }
}
