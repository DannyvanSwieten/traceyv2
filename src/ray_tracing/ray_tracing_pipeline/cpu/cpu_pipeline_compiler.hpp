#pragma once
#include <vector>
#include "../shader_binding_table.hpp"
#include "../../../device/device.hpp"

namespace tracey
{
    class CpuShaderBindingTable;
    class RayTracingPipelineLayout;
    using RayTracingEntryPointFunc = void (*)();

    struct BindingSlot
    {
        void **slotPtr;
    };

    struct CompiledShader
    {
        RayTracingEntryPointFunc sbt;
        std::vector<BindingSlot> bindingSlots;
    };

    struct Sbt
    {
        CompiledShader rayGen;
        std::vector<CompiledShader> hits;
    };

    CompiledShader
    compileCpuRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt);
    CompiledShader compileCpuShader(const RayTracingPipelineLayout &layout, ShaderStage stage, const std::string_view source, const std::string_view entryPoint);
}