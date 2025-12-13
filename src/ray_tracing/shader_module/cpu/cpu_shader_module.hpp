#pragma once
#include <string>
#include "../shader_module.hpp"
#include "../../../device/device.hpp"
namespace tracey
{
    class RayTracingPipelineLayout;
    class CpuShaderModule : public ShaderModule
    {
    public:
        CpuShaderModule(ShaderStage stage, const std::string_view source, const std::string_view entryPoint = "");

        ShaderStage stage() const;
        const std::string_view source() const;
        const std::string_view entryPoint() const;

    private:
        ShaderStage m_stage;
        std::string m_source;
        std::string m_entryPoint;
    };
} // namespace tracey