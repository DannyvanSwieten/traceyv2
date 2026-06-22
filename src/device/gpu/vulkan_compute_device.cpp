#include "vulkan_compute_device.hpp"
#include "vulkan_buffer.hpp"
#include "vulkan_image_2d.hpp"
#include "vulkan_compute_bottom_level_accelerations_structure.hpp"
#include "vulkan_compute_top_level_acceleration_structure.hpp"
#include <algorithm>
#include <sstream>
#include <array>
namespace tracey
{
    VulkanComputeDevice::VulkanComputeDevice(VulkanContext context) : m_vulkanContext(std::move(context))
    {
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // UPDATE_AFTER_BIND: required by the bindless-texture path on the
        // hit shader. FREE_DESCRIPTOR_SET: required so the per-cook
        // dispatchers (VopComputeDispatcher / CopyToPointsCompute) can
        // vkFreeDescriptorSets the transient sets they allocate every
        // dispatch. Without the latter the pool fills up after a few
        // hundred cooks and allocations start failing with
        // VK_ERROR_OUT_OF_POOL_MEMORY — surfaces as "[ctp:compute] GPU
        // dispatch failed, CPU fallback" once the pool is exhausted.
        // The two flags are independent and compatible.
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                       | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 1000;
        std::array<VkDescriptorPoolSize, 6> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[0].descriptorCount = 2048;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = 500;
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[2].descriptorCount = 100;
        poolSizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[3].descriptorCount = 256;  // Legacy combined samplers
        poolSizes[4].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        poolSizes[4].descriptorCount = 16;  // Fixed samplers (linear + nearest per set)
        poolSizes[5].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        poolSizes[5].descriptorCount = 512;  // Bindless images (256 per set * 2 sets)
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        if (vkCreateDescriptorPool(m_vulkanContext.device(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create descriptor pool");
        }

        VkCommandPoolCreateInfo cmdPoolInfo{};
        cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cmdPoolInfo.queueFamilyIndex = m_vulkanContext.computeQueueFamilyIndex();
        if (vkCreateCommandPool(m_vulkanContext.device(), &cmdPoolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create command pool");
        }

        // Create fixed samplers for bindless texture support
        VkSamplerCreateInfo linearSamplerInfo{};
        linearSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        linearSamplerInfo.magFilter = VK_FILTER_LINEAR;
        linearSamplerInfo.minFilter = VK_FILTER_LINEAR;
        linearSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        linearSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        linearSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        linearSamplerInfo.anisotropyEnable = VK_FALSE;
        linearSamplerInfo.maxAnisotropy = 1.0f;
        linearSamplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        linearSamplerInfo.unnormalizedCoordinates = VK_FALSE;
        linearSamplerInfo.compareEnable = VK_FALSE;
        linearSamplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        linearSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        if (vkCreateSampler(m_vulkanContext.device(), &linearSamplerInfo, nullptr, &m_linearSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create linear sampler");
        }

        VkSamplerCreateInfo nearestSamplerInfo = linearSamplerInfo;
        nearestSamplerInfo.magFilter = VK_FILTER_NEAREST;
        nearestSamplerInfo.minFilter = VK_FILTER_NEAREST;
        nearestSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

        if (vkCreateSampler(m_vulkanContext.device(), &nearestSamplerInfo, nullptr, &m_nearestSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create nearest sampler");
        }

        // Clamp-to-edge variants. Built from the matching filter info so the
        // only differences vs. the repeat samplers above are the address
        // modes — useful for textures whose UVs aren't tileable (decals,
        // baked AO/skydomes, some glTF assets that explicitly request clamp).
        VkSamplerCreateInfo linearClampInfo = linearSamplerInfo;
        linearClampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        linearClampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        linearClampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_vulkanContext.device(), &linearClampInfo, nullptr, &m_linearClampSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create linear clamp sampler");
        }

        VkSamplerCreateInfo nearestClampInfo = nearestSamplerInfo;
        nearestClampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearestClampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearestClampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_vulkanContext.device(), &nearestClampInfo, nullptr, &m_nearestClampSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create nearest clamp sampler");
        }
    }

    VkSampler VulkanComputeDevice::samplerForKind(SamplerKind kind) const
    {
        switch (kind)
        {
        case SamplerKind::LinearRepeat:  return m_linearSampler;
        case SamplerKind::LinearClamp:   return m_linearClampSampler;
        case SamplerKind::NearestRepeat: return m_nearestSampler;
        case SamplerKind::NearestClamp:  return m_nearestClampSampler;
        }
        return m_linearSampler;
    }

    VulkanComputeDevice::~VulkanComputeDevice()
    {
        vkDestroySampler(m_vulkanContext.device(), m_linearSampler, nullptr);
        vkDestroySampler(m_vulkanContext.device(), m_linearClampSampler, nullptr);
        vkDestroySampler(m_vulkanContext.device(), m_nearestSampler, nullptr);
        vkDestroySampler(m_vulkanContext.device(), m_nearestClampSampler, nullptr);
        vkDestroyCommandPool(m_vulkanContext.device(), m_commandPool, nullptr);
        vkDestroyDescriptorPool(m_vulkanContext.device(), m_descriptorPool, nullptr);
    }

    VkDevice VulkanComputeDevice::vkDevice() const
    {
        return m_vulkanContext.device();
    }

    Buffer *VulkanComputeDevice::createBuffer(uint32_t size, BufferUsage usageFlags)
    {
        return new VulkanBuffer(*this, size, usageFlags);
    }
    Image2D *VulkanComputeDevice::createImage2D(uint32_t width, uint32_t height, ImageFormat format)
    {
        return new VulkanImage2D(*this, width, height, format);
    }
    Image2D *VulkanComputeDevice::createImage2DWithData(uint32_t width, uint32_t height, ImageFormat format,
                                                        const void *data, size_t dataSize,
                                                        SamplerFilter filter, SamplerAddressMode addressMode)
    {
        return new VulkanImage2D(*this, width, height, format, data, dataSize, filter, addressMode);
    }
    BottomLevelAccelerationStructure *VulkanComputeDevice::createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount, const BVHConfig &bvhConfig)
    {
        return new VulkanComputeBottomLevelAccelerationStructure(*this, positions, positionCount, positionStride, indices, indexCount, bvhConfig);
    }
    TopLevelAccelerationStructure *VulkanComputeDevice::createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances)
    {
        return new VulkanComputeTopLevelAccelerationStructure(*this, blases, instances);
    }
    uint32_t VulkanComputeDevice::maxBindlessTextures() const
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_vulkanContext.physicalDevice(), &props);

        // The wavefront compute pipeline reserves ~30 fixed bindings (TLAS,
        // ray queues, payload, samplers, UBO, fixed SSBOs). Reserve 64 to keep
        // headroom for future additions without hitting maxPerStageResources.
        constexpr uint32_t kReserved = 64;
        const uint32_t budget = props.limits.maxPerStageResources > kReserved
            ? props.limits.maxPerStageResources - kReserved
            : 0;

        // Also bound by maxPerStageDescriptorSampledImages so the array itself
        // is representable on this device.
        return std::min(budget, props.limits.maxPerStageDescriptorSampledImages);
    }

    void VulkanComputeDevice::waitIdle()
    {
        // Device-wide drain: completes every queue (the compute/path-tracer
        // queue AND the editor presenter's graphics queue, which share this
        // VkDevice). Callers use this before tearing down GPU resources that
        // an in-flight command buffer might still reference.
        if (m_vulkanContext.device() != VK_NULL_HANDLE)
            vkDeviceWaitIdle(m_vulkanContext.device());
    }

    int VulkanComputeDevice::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(m_vulkanContext.physicalDevice(), &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }

        return -1;
    }
} // namespace tracey
