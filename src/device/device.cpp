#include "device.hpp"

#include "gpu/vulkan_compute_device.hpp"
#include "cpu/cpu_compute_device.hpp"

namespace tracey
{
    BufferUsage operator|(BufferUsage a, BufferUsage b)
    {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    BufferUsage &operator|=(BufferUsage &a, BufferUsage b)
    {
        a = a | b;
        return a;
    }

    Device *createDevice(DeviceType type, DeviceBackend backend)
    {
        if (type == DeviceType::Cpu)
        {
            return new CpuComputeDevice();
        }

        return new VulkanComputeDevice();
    }
}