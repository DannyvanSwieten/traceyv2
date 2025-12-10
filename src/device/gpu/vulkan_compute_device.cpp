#include "vulkan_compute_device.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
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

    RayTracingPipeline *VulkanComputeDevice::createRayTracingPipeline()
    {
        return nullptr;
    }

    ShaderModule *VulkanComputeDevice::createShaderModule(const RayTracingPipelineLayout &layout, ShaderStage stage, const std::string_view source, const std::string_view entryPoint)
    {
        std::stringstream shaderPrelude;
        shaderPrelude << "#version 450\n";
        const auto bindings = layout.bindingsForStage(stage);
        for (const auto &binding : bindings)
        {
            switch (binding.type)
            {
            case RayTracingPipelineLayout::DescriptorType::Image2D:
                shaderPrelude << "layout(binding = " << binding.index << ") uniform writeonly image2D " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayout::DescriptorType::Buffer:
                shaderPrelude << "layout(binding = " << binding.index << ") buffer " << binding.name << "Buffer {\n"
                              << "    // Define buffer structure here\n"
                              << "} " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayout::DescriptorType::AccelerationStructure:
                // Vulkan compute shaders do not support acceleration structures directly.
                // But we have our own definition for an opaque TLAS buffer.
                shaderPrelude << "layout(binding = " << binding.index << ") buffer " << binding.name << "Buffer {\n"
                              << "    // Define TLAS structure here\n"
                              << "} " << binding.name << ";\n";
                break;
            default:
                return nullptr;
            }
        }

        shaderPrelude << source;

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
        return nullptr;
    }
    BottomLevelAccelerationStructure *VulkanComputeDevice::createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount)
    {
        return nullptr;
    }
} // namespace tracey
