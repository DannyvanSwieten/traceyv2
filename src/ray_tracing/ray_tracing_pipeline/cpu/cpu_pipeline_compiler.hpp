#pragma once
#include <vector>
#include <memory>
#include "runtime/rt_symbols.h"
#include "../shader_binding_table.hpp"
#include "../../../device/device.hpp"

namespace tracey
{
    class CpuShaderBindingTable;
    class RayTracingPipelineLayout;
    using RayTracingEntryPointFunc = void (*)();

    using setBuiltinsFunc = void (*)(const rt::Builtins &b);
    using getBuiltinsFunc = void (*)(rt::Builtins *b);
    using setPayloadFunc = void (*)(rt::payload *p, unsigned int index);
    using getPayloadFunc = void (*)(rt::payload *p, unsigned int index);

    struct BindingSlot
    {
        void **slotPtr;
    };

    struct PayloadSlot
    {
        size_t payloadSize = 0;
        void *payloadPtr = nullptr;
        setPayloadFunc setPayload = nullptr;
        getPayloadFunc getPayload = nullptr;

        ~PayloadSlot()
        {
            if (payloadPtr)
            {
                std::free(payloadPtr);
                payloadPtr = nullptr;
            }
        }
    };

    struct CompiledShader
    {
        RayTracingEntryPointFunc func;
        void *dylib = nullptr;
        std::vector<BindingSlot> bindingSlots;
        std::vector<std::shared_ptr<PayloadSlot>> payloadSlots;

        void setTraceRaysExt();
    };

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