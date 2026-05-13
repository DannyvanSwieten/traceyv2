#pragma once

// Vulkan compute dispatcher for VopGraphs.
//
// Phase 2 of the GPU-VOP path. Pairs with glsl_emit (Phase 1) to take
// a VopGraph and run it per-point against a Geometry on the GPU
// instead of CPU. The pipeline cache + Vulkan plumbing lives entirely
// inside compute_dispatch.cpp — by design — so we can iterate without
// touching the abstract Device interface; graduating to a generic
// `Device::createComputePipeline()` API is a follow-up once this
// stabilises.
//
// Lifecycle:
//   • Construct one dispatcher per scene (or per process). It owns
//     compiled pipelines and reuses them across cooks.
//   • Each `dispatch(graph, geo)` call: pulls/builds the compiled
//     pipeline keyed by `hashGlsl(emit.glsl)`, uploads touched-
//     attribute arrays into transient SSBOs, packs node params into a
//     vec4 array, records a one-shot command buffer, submits +
//     waits, then reads back any `write` attributes into `geo`.
//
// Failure model: any GPU/shader error throws std::runtime_error so
// the caller can fall back to the CPU evaluator. The dispatcher does
// NOT mutate `geo` on failure (writeback only happens after the
// queue submit has signalled completion).

#include "../vop_graph.hpp"
#include "glsl_emit.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// Forward-declare the Vulkan device + buffer so this header doesn't
// pull volk in. Implementation is Vulkan-only for now.
namespace tracey
{
    class Device;
    class VulkanComputeDevice;
    class Geometry;

    namespace vops
    {
        namespace codegen
        {
            // Stats reported per dispatch for profiling. Times are
            // wall-clock; the GPU portion includes the explicit fence
            // wait, so it's a tight upper bound on actual dispatch cost.
            struct DispatchStats
            {
                double uploadMs = 0.0;
                double gpuMs = 0.0;     // shader compile + submit + wait
                double readbackMs = 0.0;
                bool   pipelineCached = false;
                size_t pointCount = 0;
            };

            class VopComputeDispatcher
            {
            public:
                // Throws if `device` is null or not (a subclass of)
                // VulkanComputeDevice. The dispatcher does NOT take
                // ownership; the caller must keep the device alive for
                // the dispatcher's lifetime.
                explicit VopComputeDispatcher(Device *device);
                ~VopComputeDispatcher();

                VopComputeDispatcher(const VopComputeDispatcher &) = delete;
                VopComputeDispatcher &operator=(const VopComputeDispatcher &) = delete;

                // Run `graph` over `geo` per-point. Reads from / writes to
                // the named attributes the emitter reports.
                //
                // Pre-condition: every attribute the graph's geo_input /
                // geo_output ports reference must exist on `geo` (the
                // dispatcher creates float attrs on demand for writes,
                // but inputs error out — same contract as the CPU eval).
                //
                // Throws std::runtime_error on shader compile error or
                // unsupported nodes; the caller should fall back to the
                // CPU evaluator and continue.
                //
                // Thread-safe: internal mutex serialises concurrent
                // calls (Vulkan command pool + descriptor pool aren't
                // safe to access from multiple threads simultaneously).
                DispatchStats dispatch(const VopGraph &graph, Geometry &geo);

                // ── Process-wide singleton accessor ──
                //
                // The cook paths (attribute_vop_sop::cookAt and
                // pop_force::cookFrame) live behind static dispatch and
                // have no `Device*` in scope — they reach the
                // dispatcher through this global. EditorServer creates
                // a dispatcher with its RenderEngine's device at
                // startup and calls setGlobal(); the cook paths call
                // getGlobal() per cook, fall back to CPU when null
                // (no GPU available, or shader compile failed).
                //
                // Lifetime: the caller of setGlobal() must keep the
                // dispatcher alive for as long as the global pointer
                // is set; clear via setGlobal(nullptr) before
                // destruction.
                static VopComputeDispatcher *getGlobal();
                static void setGlobal(VopComputeDispatcher *dispatcher);

            private:
                // One compiled pipeline + its emit metadata. Keyed by
                // hashGlsl(emit.glsl) in `m_cache`.
                struct PipelineEntry;
                PipelineEntry &compileOrGet(const VopGraph &graph);

                VulkanComputeDevice *m_device;
                // PipelineEntry is incomplete here on purpose; the cache
                // is std::unordered_map<uint64_t, std::unique_ptr<...>>
                // so deleter sees the full type in the .cpp.
                std::unordered_map<uint64_t, std::unique_ptr<PipelineEntry>> m_cache;
            };
        }
    }
}
