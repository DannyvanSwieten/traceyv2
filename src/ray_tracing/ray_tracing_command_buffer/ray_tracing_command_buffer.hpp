#pragma once
#include <cstdint>
namespace tracey
{
    class RayTracingPipeline;
    class ShaderBindingTable;
    class DescriptorSet;
    class RayTracingCommandBuffer
    {
    public:
        virtual ~RayTracingCommandBuffer() = default;

        virtual void begin() = 0;
        virtual void end() = 0;

        virtual void setPipeline(const RayTracingPipeline *pipeline) = 0;
        virtual void setDescriptorSet(const DescriptorSet *set) = 0;
        virtual void traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height) = 0;
    };
}