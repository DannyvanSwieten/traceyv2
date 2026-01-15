#pragma once
#include <span>
#include "../shader_binding_table.hpp"

namespace tracey
{
    class ShaderModule;
    class CpuShaderBindingTable : public ShaderBindingTable
    {
    public:
        CpuShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders, const std::span<const ShaderModule *> missShaders, const ShaderModule *resolveShader = nullptr);
        const ShaderModule *rayGen() const { return m_rayGen; }
        const std::span<const ShaderModule *> hitModules() const { return m_hitModules; }
        const std::span<const ShaderModule *> missModules() const { return m_missModules; }
        const ShaderModule *resolveShader() const { return m_resolveShader; }

    private:
        const ShaderModule *const m_rayGen;
        std::span<const ShaderModule *> m_hitModules;
        std::span<const ShaderModule *> m_missModules;
        const ShaderModule *const m_resolveShader;
    };
} // namespace tracey