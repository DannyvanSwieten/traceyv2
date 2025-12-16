#include "cpu_ray_tracing_pipeline.hpp"
#include "cpu_shader_binding_table.hpp"
#include "../../shader_module/cpu/cpu_shader_module.hpp"
#include "../ray_tracing_pipeline_layout.hpp"

namespace tracey
{
    CpuRayTracingPipeline::CpuRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt) : m_layout(layout), m_sbt(sbt), m_compiledSbt(compileShaders())
    {
    }
    const RayTracingPipelineLayout &CpuRayTracingPipeline::layout() const
    {
        return m_layout;
    }
    const CpuShaderBindingTable &CpuRayTracingPipeline::sbt() const
    {
        return m_sbt;
    }
    Sbt CpuRayTracingPipeline::compileShaders()
    {
        ;
        const auto rayGenModule = dynamic_cast<const CpuShaderModule *>(m_sbt.rayGen());
        Sbt sbt{RayGenShader(compileShader(*rayGenModule))};

        for (const auto &hitModulePtr : m_sbt.hitModules())
        {
            const auto hitModule = dynamic_cast<const CpuShaderModule *>(hitModulePtr);
            if (hitModule)
            {
                const auto hitFunction = compileShader(*hitModule);
                sbt.hits.emplace_back(hitFunction);
            }
        }

        return sbt;
    }
    CompiledShader CpuRayTracingPipeline::compileShader(const CpuShaderModule &module)
    {
        return compileCpuShader(m_layout, module.stage(), module.source(), module.entryPoint());
    }
}