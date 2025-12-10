#pragma once
#include <span>
#include "../shader_binding_table.hpp"

namespace tracey
{
    class ShaderModule;
    class CpuShaderBindingTable : public ShaderBindingTable
    {
    public:
        CpuShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders);

    private:
        const ShaderModule *const m_rayGen;
        std::span<const ShaderModule *> m_shaderModules;
    };
} // namespace tracey