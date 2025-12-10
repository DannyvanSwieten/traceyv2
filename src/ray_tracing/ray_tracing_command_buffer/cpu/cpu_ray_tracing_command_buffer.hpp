#pragma once

#include "../ray_tracing_command_buffer.hpp"

namespace tracey
{
    class CpuRayTracingPipeline;
    class CpuRayTracingCommandBuffer : public RayTracingCommandBuffer
    {
    public:
        CpuRayTracingCommandBuffer();

        void begin() override;
        void end() override;

        void setPipeline(const RayTracingPipeline *pipeline) override;
        void setDescriptorSet(const DescriptorSet *set) override;
        void traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height) override;

    private:
        const CpuRayTracingPipeline *m_pipeline = nullptr;
        const DescriptorSet *m_descriptorSet = nullptr;
    };
}