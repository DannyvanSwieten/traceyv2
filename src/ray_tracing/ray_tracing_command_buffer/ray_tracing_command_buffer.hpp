#pragma once
#include <cstdint>
namespace tracey
{
    class RayTracingPipeline;
    class ShaderBindingTable;
    class DescriptorSet;
    class Image2D;
    class Buffer;
    class RayTracingCommandBuffer
    {
    public:
        virtual ~RayTracingCommandBuffer() = default;

        virtual void begin() = 0;
        virtual void end() = 0;

        virtual void setPipeline(RayTracingPipeline *pipeline) = 0;
        virtual void setDescriptorSet(DescriptorSet *set) = 0;
        virtual void traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height) = 0;

        virtual void copyImageToBuffer(const Image2D *image, Buffer *buffer) = 0;

        virtual void waitUntilCompleted() = 0;
    };
}