#include "vulkan_compute_device.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_descriptor_set.hpp"
#include "vulkan_buffer.hpp"
#include "vulkan_image_2d.hpp"
#include "vulkan_compute_bottom_level_accelerations_structure.hpp"
#include <sstream>
namespace tracey
{
    VulkanComputeDevice::VulkanComputeDevice(VulkanContext context) : m_vulkanContext(std::move(context))
    {
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1000;
        std::array<VkDescriptorPoolSize, 3> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[0].descriptorCount = 2048;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = 500;
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        poolSizes[2].descriptorCount = 100;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        if (vkCreateDescriptorPool(m_vulkanContext.device(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create descriptor pool");
        }
    }

    VulkanComputeDevice::~VulkanComputeDevice()
    {
    }

    VkDevice VulkanComputeDevice::vkDevice() const
    {
        return m_vulkanContext.device();
    }

    RayTracingPipeline *VulkanComputeDevice::createRayTracingPipeline(const RayTracingPipelineLayout &layout, const ShaderBindingTable *sbt)
    {
        return nullptr;
    }

    ShaderModule *VulkanComputeDevice::createShaderModule(ShaderStage stage, const std::string_view source, const std::string_view entryPoint)
    {
        return nullptr;
    }
    ShaderBindingTable *VulkanComputeDevice::createShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders, const std::span<const ShaderModule *> missShaders)
    {
        return nullptr;
    }
    RayTracingCommandBuffer *VulkanComputeDevice::createRayTracingCommandBuffer()
    {
        return nullptr;
    }
    void VulkanComputeDevice::allocateDescriptorSets(std::span<DescriptorSet *> sets, const RayTracingPipelineLayout &layout)
    {
        for (auto &set : sets)
        {
            auto vkSet = new VulkanComputeRayTracingDescriptorSet(*this, layout);
            set = vkSet;
        }
    }
    Buffer *VulkanComputeDevice::createBuffer(uint32_t size, BufferUsage usageFlags)
    {
        return new VulkanBuffer(*this, size, usageFlags);
    }
    Image2D *VulkanComputeDevice::createImage2D(uint32_t width, uint32_t height, ImageFormat format)
    {
        return new VulkanImage2D(*this, width, height, format);
    }
    BottomLevelAccelerationStructure *VulkanComputeDevice::createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount)
    {
        return new VulkanComputeBottomLevelAccelerationStructure(*this, positions, positionCount, positionStride, indices, indexCount);
    }
    TopLevelAccelerationStructure *VulkanComputeDevice::createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const struct Tlas::Instance> instances)
    {
        return nullptr;
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
