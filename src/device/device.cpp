#include "device.hpp"

#include "gpu/vulkan_compute_device.hpp"
#include "cpu/cpu_compute_device.hpp"

namespace tracey
{
    Device *createDevice(DeviceType type, DeviceBackend backend)
    {
        if (type == DeviceType::Cpu)
        {
            return new CpuComputeDevice();
        }

        return new VulkanComputeDevice();
    }
}