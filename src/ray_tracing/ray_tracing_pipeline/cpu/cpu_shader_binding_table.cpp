#include "cpu_shader_binding_table.hpp"

namespace tracey
{
    CpuShaderBindingTable::CpuShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders) : m_rayGen(rayGen), m_hitModules(hitShaders)
    {
    }
}