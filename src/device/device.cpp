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
        return createDevice(type, backend, /*enablePresentation=*/false);
    }

    Device *createDevice(DeviceType type, DeviceBackend backend, bool enablePresentation)
    {
        (void)backend;
        if (type == DeviceType::Cpu)
        {
            return new CpuComputeDevice();
        }

        VulkanContextConfig cfg;
        cfg.enablePresentation = enablePresentation;
        return new VulkanComputeDevice(VulkanContext{cfg});
    }
}