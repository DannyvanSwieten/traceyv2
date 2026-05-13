#include "vulkan_queue_sync.hpp"

namespace tracey
{
    std::mutex &vulkanQueueMutex()
    {
        // Function-local static: lazily constructed, lives for the
        // process lifetime, never destructed (or destructed after main
        // exits — either way the queue is gone by then).
        static std::mutex m;
        return m;
    }
}
