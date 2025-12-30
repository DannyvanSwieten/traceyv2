#include "ray_tracing_pipeline_layout.hpp"
namespace tracey
{
    void RayTracingPipelineLayoutDescriptor::addImage2D(std::string name, uint32_t index, ShaderStage stage)
    {
        m_bindings.emplace_back(DescriptorBinding{name, index, DescriptorType::Image2D, stage});
    }

    void RayTracingPipelineLayoutDescriptor::addBuffer(std::string name, uint32_t index, ShaderStage stage, const StructureLayout &structure)
    {
        m_bindings.emplace_back(DescriptorBinding{name, index, DescriptorType::Buffer, stage, structure});
    }
    void RayTracingPipelineLayoutDescriptor::addAccelerationStructure(std::string name, uint32_t index, ShaderStage stage)
    {
        m_bindings.emplace_back(DescriptorBinding{name, index, DescriptorType::AccelerationStructure, stage});
    }
    void RayTracingPipelineLayoutDescriptor::addPayload(std::string name, uint32_t index, ShaderStage stage, const StructureLayout &structure)
    {
        m_payloads.emplace_back(PayloadBinding{name, index, stage, structure});
    }
    std::vector<RayTracingPipelineLayoutDescriptor::DescriptorBinding> RayTracingPipelineLayoutDescriptor::bindingsForStage(ShaderStage stage) const
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
    std::vector<RayTracingPipelineLayoutDescriptor::PayloadBinding> RayTracingPipelineLayoutDescriptor::payloadsForStage(ShaderStage stage) const
    {
        std::vector<PayloadBinding> result;
        for (const auto &payload : m_payloads)
        {
            if (payload.stage == stage)
            {
                result.emplace_back(payload);
            }
        }
        return result;
    }
}