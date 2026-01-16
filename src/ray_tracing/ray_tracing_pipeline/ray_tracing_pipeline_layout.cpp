#include "ray_tracing_pipeline_layout.hpp"
namespace tracey
{
    void RayTracingPipelineLayoutDescriptor::addImage2D(std::string name, ShaderStage stage)
    {
        m_bindings.emplace_back(DescriptorBinding{name, DescriptorType::Image2D, stage});
    }

    void RayTracingPipelineLayoutDescriptor::addStorageBuffer(std::string name, ShaderStage stage, const StructureLayout &structure)
    {
        m_bindings.emplace_back(DescriptorBinding{name, DescriptorType::StorageBuffer, stage, structure});
    }
    void RayTracingPipelineLayoutDescriptor::addUniformBuffer(std::string name, ShaderStage stage, const StructureLayout &structure)
    {
        m_bindings.emplace_back(DescriptorBinding{name, DescriptorType::UniformBuffer, stage, structure});
    }
    void RayTracingPipelineLayoutDescriptor::addAccelerationStructure(std::string name, ShaderStage stage)
    {
        m_bindings.insert(m_bindings.begin(), DescriptorBinding{name, DescriptorType::AccelerationStructure, stage});
    }
    void RayTracingPipelineLayoutDescriptor::addPayload(std::string name, ShaderStage stage, const StructureLayout &structure)
    {
        m_payloads.emplace_back(PayloadBinding{name, stage, structure});
    }
    size_t RayTracingPipelineLayoutDescriptor::indexForBinding(const std::string_view name) const
    {
        for (size_t i = 0; i < m_bindings.size(); ++i)
        {
            if (m_bindings[i].name == name)
            {
                return i;
            }
        }
        throw std::runtime_error("Binding with name '" + std::string{name} + "' not found.");
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