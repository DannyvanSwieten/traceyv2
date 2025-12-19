#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include "../core/tlas.hpp"

namespace tracey
{
    enum class DeviceType
    {
        Cpu,
        Gpu
    };

    enum class DeviceBackend
    {
        None,
        Compute,
        Rtx,
    };

    enum class ShaderStage
    {
        RayGeneration,
        Miss,
        ClosestHit,
        AnyHit,
        Intersection
    };

    enum class BufferUsage : uint32_t
    {
        None = 0,
        VertexBuffer = 1 << 0,
        IndexBuffer = 1 << 1,
        StorageBuffer = 1 << 2,
        AccelerationStructureBuildInput = 1 << 3,
        AccelerationStructureStorage = 1 << 4,
    };

    BufferUsage operator|(BufferUsage a, BufferUsage b);
    BufferUsage &operator|=(BufferUsage &a, BufferUsage b);

    enum class ImageFormat
    {
        R8G8B8A8Unorm,
        R32G32B32A32Sfloat,
        R32Sfloat,
    };

    class BottomLevelAccelerationStructure;
    class TopLevelAccelerationStructure;
    class RayTracingPipeline;
    class RayTracingPipelineLayout;
    class ShaderModule;
    class ShaderBindingTable;
    class RayTracingCommandBuffer;
    class DescriptorSet;
    class Buffer;
    class Image2D;
    class Device
    {
    public:
        virtual ~Device() = default;
        virtual RayTracingPipeline *createRayTracingPipeline(const RayTracingPipelineLayout &layout, const ShaderBindingTable *sbt) = 0;
        virtual ShaderModule *createShaderModule(ShaderStage stage, const std::string_view source, const std::string_view entryPoint) = 0;
        virtual ShaderBindingTable *createShaderBindingTable(const ShaderModule *rayGen, const std::span<const ShaderModule *> hitShaders, const std::span<const ShaderModule *> missShaders) = 0;
        virtual RayTracingCommandBuffer *createRayTracingCommandBuffer() = 0;
        virtual void allocateDescriptorSets(std::span<DescriptorSet *> sets, const RayTracingPipelineLayout &layout) = 0;
        virtual Buffer *createBuffer(uint32_t size, BufferUsage usageFlags) = 0;
        virtual Image2D *createImage2D(uint32_t width, uint32_t height, ImageFormat format) = 0;
        virtual BottomLevelAccelerationStructure *createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount) = 0;
        virtual TopLevelAccelerationStructure *createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances) = 0;
    };

    Device *createDevice(DeviceType type, DeviceBackend backend);
}