#pragma once
#include <vector>
#include "../ray_tracing_command_buffer.hpp"

namespace tracey
{
    class CpuRayTracingPipeline;
    class CpuDescriptorSet;
    class TopLevelAccelerationStructure;
    class CpuRayTracingCommandBuffer : public RayTracingCommandBuffer
    {
    public:
        CpuRayTracingCommandBuffer();

        void begin() override;
        void end() override;

        void setPipeline(RayTracingPipeline *pipeline) override;
        void setDescriptorSet(DescriptorSet *set) override;
        void traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height,
                       const TraceRaysParams &params = TraceRaysParams{}) override;
        void traceSingleSample(const ShaderBindingTable &sbt, uint32_t width, uint32_t height,
                              const TraceSingleSampleParams &params) override;
        void copyImageToBuffer(const Image2D *image, Buffer *buffer) override;
        void clearImage(Image2D *image, float r, float g, float b, float a) override;

        void waitUntilCompleted() override;
        void submitAsync() override;
        bool isCompleted() const override;

    private:
        CpuRayTracingPipeline *m_pipeline = nullptr;
        CpuDescriptorSet *m_descriptorSet = nullptr;
    };
}