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
        void traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height) override;

    private:
        CpuRayTracingPipeline *m_pipeline = nullptr;
        CpuDescriptorSet *m_descriptorSet = nullptr;
    };
}