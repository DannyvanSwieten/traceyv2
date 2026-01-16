#include "vulkan_compute_device.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_descriptor_set.hpp"
#include "vulkan_buffer.hpp"
#include "vulkan_image_2d.hpp"
#include "vulkan_compute_bottom_level_accelerations_structure.hpp"
#include "vulkan_compute_top_level_acceleration_structure.hpp"
#include "../../ray_tracing/shader_module/cpu/cpu_shader_module.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/cpu/cpu_shader_binding_table.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_pipeline.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/gpu/wavefront/vulkan_wavefront_pipeline.hpp"
#include "../../ray_tracing/ray_tracing_command_buffer/gpu/vulkan_compute_ray_tracing_command_buffer.hpp"
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
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[2].descriptorCount = 100;
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
    }

    VulkanComputeDevice::~VulkanComputeDevice()
    {
        vkDestroyCommandPool(m_vulkanContext.device(), m_commandPool, nullptr);
        vkDestroyDescriptorPool(m_vulkanContext.device(), m_descriptorPool, nullptr);
    }

    VkDevice VulkanComputeDevice::vkDevice() const
    {
        return m_vulkanContext.device();
    }

    RayTracingPipeline *VulkanComputeDevice::createRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const ShaderBindingTable *sbt)
    {
        return new VulkanComputeRaytracingPipeline(*this, layout, *dynamic_cast<const CpuShaderBindingTable *>(sbt));
    }

    RayTracingPipeline *VulkanComputeDevice::createWaveFrontRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const ShaderBindingTable *sbt)
    {
        fprintf(stderr, "\n=== createWaveFrontRayTracingPipeline called ===\n");
        fflush(stderr);
        auto* pipeline = new VulkanWaveFrontPipeline(*this, layout, *dynamic_cast<const CpuShaderBindingTable *>(sbt));
        fprintf(stderr, "=== VulkanWaveFrontPipeline created successfully ===\n");
        fflush(stderr);
        return pipeline;
    }

    ShaderModule *VulkanComputeDevice::createShaderModule(ShaderStage stage, const std::string_view source, const std::string_view entryPoint)
    {
        return new CpuShaderModule(stage, source, entryPoint);
    }
    ShaderBindingTable *VulkanComputeDevice::createShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders, const std::span<const ShaderModule *> missShaders, const ShaderModule *resolveShader)
    {
        return new CpuShaderBindingTable(rayGen, hitShaders, missShaders, resolveShader);
    }
    RayTracingCommandBuffer *VulkanComputeDevice::createRayTracingCommandBuffer()
    {
        return new VulkanComputeRayTracingCommandBuffer(*this);
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
    TopLevelAccelerationStructure *VulkanComputeDevice::createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances)
    {
        return new VulkanComputeTopLevelAccelerationStructure(*this, blases, instances);
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
