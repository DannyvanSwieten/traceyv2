#pragma once
#include "../device.hpp"
#include "../../core/tlas.hpp"
namespace tracey
{
    class CpuComputeDevice : public Device
    {
    public:
        CpuComputeDevice();
        ~CpuComputeDevice();

        // Synchronization (no-op for CPU)
        void waitIdle() override {}

        // Ray tracing
        RayTracingPipeline *createRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const ShaderBindingTable *sbt) override;
        RayTracingPipeline *createWaveFrontRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const ShaderBindingTable *sbt) override;
        ShaderModule *createShaderModule(ShaderStage stage, const std::string_view source, const std::string_view entryPoint) override;
        ShaderBindingTable *createShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders, const std::span<const ShaderModule *> missShaders, const ShaderModule *resolveShader = nullptr) override;
        RayTracingCommandBuffer *createRayTracingCommandBuffer() override;

        // Graphics (not supported on CPU device)
        GraphicsPipeline *createGraphicsPipeline(const GraphicsPipelineConfig &config, const GraphicsPipelineLayout &layout) override;
        GraphicsCommandBuffer *createGraphicsCommandBuffer() override;
        Buffer *createBuffer(uint32_t size, BufferUsage usageFlags) override;
        Image2D *createImage2D(uint32_t width, uint32_t height, ImageFormat format) override;
        Image2D *createImage2DWithData(uint32_t width, uint32_t height, ImageFormat format,
                                       const void *data, size_t dataSize,
                                       SamplerFilter filter = SamplerFilter::Linear,
                                       SamplerAddressMode addressMode = SamplerAddressMode::Repeat) override;
        BottomLevelAccelerationStructure *createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount, const BVHConfig &bvhConfig = {}) override;
        TopLevelAccelerationStructure *createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const struct Tlas::Instance> instances) override;
    };
}