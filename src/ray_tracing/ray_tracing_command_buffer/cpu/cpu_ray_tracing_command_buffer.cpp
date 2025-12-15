#include "cpu_ray_tracing_command_buffer.hpp"
#include "../../ray_tracing_pipeline/cpu/cpu_ray_tracing_pipeline.hpp"
#include "../../ray_tracing_pipeline/cpu/cpu_descriptor_set.hpp"
#include "../../ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include <cassert>
namespace tracey
{
    CpuRayTracingCommandBuffer::CpuRayTracingCommandBuffer()
    {
    }
    void CpuRayTracingCommandBuffer::begin()
    {
    }

    void CpuRayTracingCommandBuffer::end()
    {
    }

    void CpuRayTracingCommandBuffer::setPipeline(RayTracingPipeline *pipeline)
    {
        m_pipeline = dynamic_cast<CpuRayTracingPipeline *>(pipeline);
    }
    void CpuRayTracingCommandBuffer::setDescriptorSet(DescriptorSet *set)
    {
        m_descriptorSet = dynamic_cast<CpuDescriptorSet *>(set);
        for (size_t i = 0; i < m_pipeline->layout().bindings().size(); ++i)
        {
            const auto &binding = m_pipeline->layout().bindings()[i];
            if (binding.type == RayTracingPipelineLayout::DescriptorType::AccelerationStructure)
            {
                // Set the pipeline pointer in the DispatchedTlas
                m_descriptorSet->visitMut([this](auto &&arg)
                                          {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, DispatchedTlas >)
                    {
                        arg.pipeline = m_pipeline;
                    } },
                                          binding.index);
            }
        }
    }
    void CpuRayTracingCommandBuffer::traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height)
    {
        if (!m_pipeline)
        {
            assert(false && "Pipeline not set for CpuRayTracingCommandBuffer");
            return;
        }

        auto compiledSbt = m_pipeline->compiledSbt();
        const auto &descriptors = m_pipeline->layout().bindings();
        for (const auto &binding : descriptors)
        {
            // Binding setup logic would go here
            if (binding.stage == ShaderStage::RayGeneration)
            {
                // Setup for ray generation shader
                m_descriptorSet->visitMut([&](auto &&arg)
                                          {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, Image2D *>)
                    {
                        *m_pipeline->compiledSbt().rayGen.shader.bindingSlots[binding.index].slotPtr = arg;
                    }
                    else if constexpr (std::is_same_v<T, Buffer *>)
                    {
                        // Setup buffer
                    }
                    else if constexpr (std::is_same_v<T,  DispatchedTlas>)
                    {
                        *m_pipeline->compiledSbt().rayGen.shader.bindingSlots[binding.index].slotPtr = &arg;
                    } },
                                          binding.index);
            }
            else if (binding.stage == ShaderStage::ClosestHit)
            {
                // Setup for hit shaders
            }
        }

        // CPU-based ray tracing execution logic would go here
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                // Invoke ray generation shader function
                if (compiledSbt.rayGen.shader.func)
                {
                    rt::Builtins builtins;
                    builtins.glLaunchIDEXT = {x, y, 0};
                    builtins.glLaunchSizeEXT = {width, height, 1};
                    if (compiledSbt.rayGen.setBuiltins)
                    {
                        compiledSbt.rayGen.setBuiltins(builtins);
                    }
                }
                compiledSbt.rayGen.shader.func();
            }
        }
    }
}
