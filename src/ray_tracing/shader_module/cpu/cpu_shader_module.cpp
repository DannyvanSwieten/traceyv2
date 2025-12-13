#include "cpu_shader_module.hpp"
#include "../../ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"

namespace tracey
{
    CpuShaderModule::CpuShaderModule(ShaderStage stage, const std::string_view source, const std::string_view entryPoint) : m_stage(stage), m_source(source), m_entryPoint(entryPoint)
    {
    }

    ShaderStage CpuShaderModule::stage() const
    {
        return m_stage;
    }

    const std::string_view CpuShaderModule::source() const
    {
        return m_source;
    }
    const std::string_view CpuShaderModule::entryPoint() const
    {
        return m_entryPoint;
    }
}