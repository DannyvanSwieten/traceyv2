#include "ray_tracing_pipeline_layout.hpp"
namespace tracey
{
    void RayTracingPipelineLayout::addImage2D(std::string name, uint32_t index, ShaderStage stage)
    {
        m_bindings.emplace_back(DescriptorBinding{name, index, DescriptorType::Image2D, stage});
    }

    void RayTracingPipelineLayout::addBuffer(std::string name, uint32_t index, ShaderStage stage)
    {
        m_bindings.emplace_back(DescriptorBinding{name, index, DescriptorType::Buffer, stage});
    }
    void RayTracingPipelineLayout::addAccelerationStructure(std::string name, uint32_t index, ShaderStage stage)
    {
        m_bindings.emplace_back(DescriptorBinding{name, index, DescriptorType::AccelerationStructure, stage});
    }
    std::vector<RayTracingPipelineLayout::DescriptorBinding> RayTracingPipelineLayout::bindingsForStage(ShaderStage stage) const
    {
        std::vector<DescriptorBinding> result;
        for (const auto &binding : m_bindings)
        {
            if (binding.stage == stage)
            {
                result.emplace_back(binding);
            }
        }
        return result;
    }
}