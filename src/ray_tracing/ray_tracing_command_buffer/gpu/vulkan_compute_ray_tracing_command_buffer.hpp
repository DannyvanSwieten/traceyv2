#pragma once
#include <volk.h>
#include "../ray_tracing_command_buffer.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class VulkanComputeRayTracingCommandBuffer : public RayTracingCommandBuffer
    {
    public:
        VulkanComputeRayTracingCommandBuffer(VulkanComputeDevice &device);
        ~VulkanComputeRayTracingCommandBuffer() override;

        VkCommandBuffer vkCommandBuffer() const { return m_vkCommandBuffer; }

        void begin() override;
        void end() override;
        void setPipeline(RayTracingPipeline *pipeline) override;
        void setDescriptorSet(DescriptorSet *set) override;
        void traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height) override;
        void copyImageToBuffer(const Image2D *image, Buffer *buffer) override;

        void waitUntilCompleted() override;

    private:
        VulkanComputeDevice &m_device;
        VkCommandBuffer m_vkCommandBuffer;
        RayTracingPipeline *m_pipeline = nullptr;
    };
} // namespace tracey