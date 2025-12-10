#include "cpu_compute_device.hpp"
#include "cpu_buffer.hpp"
#include "cpu_bottom_level_acceleration_structure.hpp"
#include "../../ray_tracing/shader_module/cpu/cpu_shader_module.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/cpu/cpu_shader_binding_table.hpp"
#include "../../ray_tracing/ray_tracing_pipeline/cpu/cpu_descriptor_set.hpp"
#include <sstream>
namespace tracey
{
    CpuComputeDevice::CpuComputeDevice()
    {
    }

    CpuComputeDevice::~CpuComputeDevice()
    {
    }

    RayTracingPipeline *CpuComputeDevice::createRayTracingPipeline()
    {
        return nullptr;
    }

    ShaderModule *CpuComputeDevice::createShaderModule(const RayTracingPipelineLayout &layout, ShaderStage stage, const std::string_view source, const std::string_view entryPoint)
    {
        return new CpuShaderModule(layout, stage, source, entryPoint);
    }
    ShaderBindingTable *CpuComputeDevice::createShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders)
    {
        return new CpuShaderBindingTable(rayGen, hitShaders);
    }
    RayTracingCommandBuffer *CpuComputeDevice::createRayTracingCommandBuffer()
    {
        return nullptr;
    }
    void CpuComputeDevice::allocateDescriptorSets(std::span<DescriptorSet *> sets, const RayTracingPipelineLayout &layout)
    {
        for (auto &set : sets)
        {
            set = new CpuDescriptorSet(layout);
        }
    }
    Buffer *CpuComputeDevice::createBuffer(uint32_t size, BufferUsage usageFlags)
    {
        return new CpuBuffer(size);
    }
    BottomLevelAccelerationStructure *CpuComputeDevice::createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount)
    {
        return new CpuBottomLevelAccelerationStructure(positions, positionCount, positionStride, indices, indexCount);
    }
} // namespace tracey