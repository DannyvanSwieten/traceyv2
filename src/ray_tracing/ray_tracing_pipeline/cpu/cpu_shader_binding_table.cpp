#include "cpu_shader_binding_table.hpp"

namespace tracey
{
    CpuShaderBindingTable::CpuShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders, const std::span<const ShaderModule *> missShaders, const ShaderModule *resolveShader) : m_rayGen(rayGen), m_hitModules(hitShaders), m_missModules(missShaders), m_resolveShader(resolveShader)
    {
    }
}