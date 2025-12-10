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
        CpuShaderModule(const RayTracingPipelineLayout &layout, ShaderStage stage, const std::string_view source, const std::string_view entryPoint = "");

    private:
        std::string m_source;
        void *m_dylibHandle = nullptr;
        void (*m_entryPointFunc)() = nullptr;
    };
} // namespace tracey