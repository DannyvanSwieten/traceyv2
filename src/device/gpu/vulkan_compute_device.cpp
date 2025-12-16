#include "vulkan_compute_device.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "vulkan_buffer.hpp"
#include "vulkan_image_2d.hpp"
#include <sstream>
namespace tracey
{
    VulkanComputeDevice::VulkanComputeDevice()
    {
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
    ShaderBindingTable *VulkanComputeDevice::createShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders)
    {
        return nullptr;
    }
    RayTracingCommandBuffer *VulkanComputeDevice::createRayTracingCommandBuffer()
    {
        return nullptr;
    }
    void VulkanComputeDevice::allocateDescriptorSets(std::span<DescriptorSet *> sets, const RayTracingPipelineLayout &layout)
    {
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
        return nullptr;
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
