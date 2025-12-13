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
        const ShaderModule *rayGen() const { return m_rayGen; }
        const std::span<const ShaderModule *> hitModules() const { return m_hitModules; }

    private:
        const ShaderModule *const m_rayGen;
        std::span<const ShaderModule *> m_hitModules;
    };
} // namespace tracey