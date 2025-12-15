#pragma once
#include <vector>
#include "runtime/rt_symbols.h"
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
        RayTracingEntryPointFunc func;
        void *dylib = nullptr;
        std::vector<BindingSlot> bindingSlots;

        void setTraceRaysExt();
    };

    using setBuiltinsFunc = void (*)(const rt::Builtins &b);
    using getBuiltinsFunc = void (*)(rt::Builtins *b);

    struct RayGenShader
    {
        RayGenShader(CompiledShader shader);
        CompiledShader shader;
        setBuiltinsFunc setBuiltins = nullptr;
        getBuiltinsFunc getBuiltins = nullptr;
    };

    struct ClosestHitShader
    {
        ClosestHitShader(CompiledShader shader);
        CompiledShader shader;
        setBuiltinsFunc setBuiltins = nullptr;
        getBuiltinsFunc getBuiltins = nullptr;
    };

    struct Sbt
    {
        Sbt(RayGenShader rayGenShader);
        RayGenShader rayGen;
        std::vector<ClosestHitShader> hits;
    };

    CompiledShader
    compileCpuRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt);
    CompiledShader compileCpuShader(const RayTracingPipelineLayout &layout, ShaderStage stage, const std::string_view source, const std::string_view entryPoint);
}