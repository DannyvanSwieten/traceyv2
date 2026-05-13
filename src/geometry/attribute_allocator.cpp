#include "attribute_allocator.hpp"

#include <atomic>

namespace tracey
{
    namespace
    {
        // Plain pointer (raw, non-owning). Storing as atomic<Device*>
        // so concurrent readers (cook worker, render thread, GPU
        // dispatcher) can observe set/clear races without UB.
        std::atomic<Device *> g_device{nullptr};
    }

    Device *AttributeAllocator::getDevice()
    {
        return g_device.load(std::memory_order_acquire);
    }

    void AttributeAllocator::setDevice(Device *device)
    {
        g_device.store(device, std::memory_order_release);
    }
}
