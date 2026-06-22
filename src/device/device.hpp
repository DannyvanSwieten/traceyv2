#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include "../core/tlas.hpp"
#include "../core/blas.hpp"

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
        Intersection,
        Resolve
    };

    enum class BufferUsage : uint32_t
    {
        None = 0,
        VertexBuffer = 1 << 0,
        IndexBuffer = 1 << 1,
        StorageBuffer = 1 << 2,
        AccelerationStructureBuildInput = 1 << 3,
        AccelerationStructureStorage = 1 << 4,
        UniformBuffer = 1 << 5,
        TransferSrc = 1 << 6,
        TransferDst = 1 << 7,
    };

    BufferUsage operator|(BufferUsage a, BufferUsage b);
    BufferUsage &operator|=(BufferUsage &a, BufferUsage b);

    enum class ImageFormat
    {
        R8G8B8A8Unorm,
        R8G8B8A8Srgb,
        R32G32B32A32Sfloat,
        R32Sfloat,
    };

    enum class SamplerFilter
    {
        Nearest,
        Linear
    };

    enum class SamplerAddressMode
    {
        Repeat,
        ClampToEdge,
        MirroredRepeat
    };

    // Four standard sampler presets the renderer keeps as descriptor-set
    // entries. The hit shader picks one per texture access via packed bits
    // on GPUMaterial. Keeping this enum tight (2 bits per slot) lets us
    // pack five per-material sampler choices into a single uint.
    enum class SamplerKind : uint8_t
    {
        LinearRepeat  = 0,
        LinearClamp   = 1,
        NearestRepeat = 2,
        NearestClamp  = 3,
    };

    class BottomLevelAccelerationStructure;
    class TopLevelAccelerationStructure;
    class Buffer;
    class Image2D;
    class Device
    {
    public:
        virtual ~Device() = default;
        virtual Buffer *createBuffer(uint32_t size, BufferUsage usageFlags) = 0;
        virtual Image2D *createImage2D(uint32_t width, uint32_t height, ImageFormat format) = 0;
        virtual Image2D *createImage2DWithData(uint32_t width, uint32_t height, ImageFormat format,
                                               const void *data, size_t dataSize,
                                               SamplerFilter filter = SamplerFilter::Linear,
                                               SamplerAddressMode addressMode = SamplerAddressMode::Repeat) = 0;
        virtual BottomLevelAccelerationStructure *createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount, const BVHConfig &bvhConfig = {}) = 0;
        virtual TopLevelAccelerationStructure *createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const Tlas::Instance> instances) = 0;

        // Upper bound on bindless sampled-image array size for a single
        // descriptor set. Backends derive this from physical device limits
        // (e.g. maxPerStageResources on Vulkan/MoltenVK, where the entire
        // compute pipeline's resource count must fit).
        virtual uint32_t maxBindlessTextures() const = 0;

        // Block until ALL GPU work submitted to this device has completed.
        // Call before destroying/recreating GPU resources that an in-flight
        // command buffer may still reference — e.g. recreating the path tracer
        // on a resolution change while the viewport presenter's blit of the old
        // output image is still executing on the GPU. Destroying the image then
        // is use-after-free → "vkDestroyImage ... in use" → device loss. No-op
        // on devices with no async GPU queue (CPU).
        virtual void waitIdle() {}
    };

    Device *createDevice(DeviceType type, DeviceBackend backend);
    Device *createDevice(DeviceType type, DeviceBackend backend, bool enablePresentation);
}