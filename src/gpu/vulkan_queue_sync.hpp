#pragma once

#include <mutex>

namespace tracey
{
    // Process-wide GPU command lock. Held across every access to the
    // shared VkCommandPool *and* every vkQueueSubmit / vkQueuePresentKHR.
    //
    // Vulkan requires external synchronisation on BOTH:
    //   • `VkQueue` — concurrent submits/presents from different threads
    //     are undefined, validation reports "THREADING ERROR: object of
    //     type VkQueue is simultaneously used in current thread X and
    //     thread Y".
    //   • `VkCommandPool` and any command buffer allocated from it —
    //     command recording (`vkCmdXXX`), allocation
    //     (`vkAllocateCommandBuffers`), freeing, and reset all share the
    //     pool's threading domain. The same validator catches this with
    //     "THREADING ERROR: object of type VkCommandPool is
    //     simultaneously used in current thread X and thread Y" and
    //     paired `vkCmdPipelineBarrier` complaints.
    //
    // Tracey's cook worker thread runs the VOP / SOP compute
    // dispatchers while the render thread submits path-tracer and
    // rasterizer commands plus the presenter's composite — all
    // targeting the same `VkQueue` and `VkCommandPool` in our
    // VulkanContext + VulkanComputeDevice. A single global mutex held
    // across each subsystem's entire record-and-submit critical section
    // serialises them without per-subsystem coordination.
    //
    // Coarse locking trades parallelism for correctness: cook and
    // render threads serialise their entire GPU work, including the
    // fence wait. If contention becomes a bottleneck the right
    // structural fix is per-thread command pools (each thread records
    // independently; only the queue submit takes a finer mutex). The
    // mutex stays process-wide rather than per-device because we only
    // instantiate one VulkanComputeDevice in this editor and threading
    // the device handle through every submitter would add a lot of
    // plumbing for no additional separation.
    std::mutex &vulkanQueueMutex();
}
