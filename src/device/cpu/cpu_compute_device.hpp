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

        Buffer *createBuffer(uint32_t size, BufferUsage usageFlags) override;
        Image2D *createImage2D(uint32_t width, uint32_t height, ImageFormat format) override;
        Image2D *createImage2DWithData(uint32_t width, uint32_t height, ImageFormat format,
                                       const void *data, size_t dataSize,
                                       SamplerFilter filter = SamplerFilter::Linear,
                                       SamplerAddressMode addressMode = SamplerAddressMode::Repeat) override;
        BottomLevelAccelerationStructure *createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount, const BVHConfig &bvhConfig = {}) override;
        TopLevelAccelerationStructure *createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const struct Tlas::Instance> instances) override;
        uint32_t maxBindlessTextures() const override { return 4096; }
    };
}