#pragma once
#include <cstdint>
namespace tracey
{
    class RayTracingPipeline;
    class ShaderBindingTable;
    class DescriptorSet;
    class Image2D;
    class Buffer;

    /// Parameters for ray tracing dispatch
    struct TraceRaysParams
    {
        uint32_t samplesPerFrame = 16;
        uint32_t maxBounces = 8;
        uint32_t baseSampleCount = 0;  // For resolve shader accumulation
    };

    /// Parameters for single-sample ray tracing (progressive rendering)
    struct TraceSingleSampleParams
    {
        uint32_t globalSampleIndex = 0;  // Which sample globally (for RNG seeding)
        uint32_t baseSampleCount = 0;    // Previous accumulated samples
        uint32_t maxBounces = 8;         // Ray depth limit
    };

    class RayTracingCommandBuffer
    {
    public:
        virtual ~RayTracingCommandBuffer() = default;

        virtual void begin() = 0;
        virtual void end() = 0;

        virtual void setPipeline(RayTracingPipeline *pipeline) = 0;
        virtual void setDescriptorSet(DescriptorSet *set) = 0;
        virtual void traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height,
                               const TraceRaysParams &params = TraceRaysParams{}) = 0;

        /// Trace a single sample (for progressive rendering)
        virtual void traceSingleSample(const ShaderBindingTable &sbt, uint32_t width, uint32_t height,
                                      const TraceSingleSampleParams &params) = 0;

        virtual void copyImageToBuffer(const Image2D *image, Buffer *buffer) = 0;
        virtual void clearImage(Image2D *image, float r, float g, float b, float a) = 0;

        virtual void waitUntilCompleted() = 0;

        /// Submit command buffer without blocking
        virtual void submitAsync() = 0;

        /// Check if command buffer execution has completed (non-blocking)
        virtual bool isCompleted() const = 0;
    };
}