#include "cpu_compute_device.hpp"
#include "cpu_buffer.hpp"
#include "cpu_image_2d.hpp"
#include "cpu_bottom_level_acceleration_structure.hpp"
#include "../../device/cpu/cpu_top_level_acceleration_structure.hpp"
#include <sstream>
namespace tracey
{
    CpuComputeDevice::CpuComputeDevice()
    {
    }

    CpuComputeDevice::~CpuComputeDevice()
    {
    }

    // void CpuComputeDevice::allocateDescriptorSets(std::span<DescriptorSet *> sets, const RayTracingPipelineLayoutDescriptor &layout)
    // {
    //     for (auto &set : sets)
    //     {
    //         set = new CpuDescriptorSet(layout);
    //     }
    // }
    Buffer *CpuComputeDevice::createBuffer(uint32_t size, BufferUsage /*usageFlags*/)
    {
        return new CpuBuffer(size);
    }
    Image2D *CpuComputeDevice::createImage2D(uint32_t width, uint32_t height, ImageFormat format)
    {
        return new CpuImage2D(width, height, format);
    }
    Image2D *CpuComputeDevice::createImage2DWithData(uint32_t width, uint32_t height, ImageFormat format,
                                                     const void *data, size_t dataSize,
                                                     SamplerFilter /*filter*/, SamplerAddressMode /*addressMode*/)
    {
        return new CpuImage2D(width, height, format, data, dataSize);
    }
    BottomLevelAccelerationStructure *CpuComputeDevice::createBottomLevelAccelerationStructure(const Buffer *positions, uint32_t positionCount, uint32_t positionStride, const Buffer *indices, uint32_t indexCount, const BVHConfig &bvhConfig)
    {
        return new CpuBottomLevelAccelerationStructure(positions, positionCount, positionStride, indices, indexCount, bvhConfig);
    }
    TopLevelAccelerationStructure *CpuComputeDevice::createTopLevelAccelerationStructure(std::span<const BottomLevelAccelerationStructure *> blases, std::span<const struct Tlas::Instance> instances)
    {
        return new CpuTopLevelAccelerationStructure(blases, instances);
    }
} // namespace tracey