#pragma once
#include <cstddef>

namespace tracey
{
    class Blas;

    class BottomLevelAccelerationStructure
    {
    public:
        virtual ~BottomLevelAccelerationStructure() = default;

        /// Get the number of BVH nodes (for statistics/debugging)
        virtual size_t nodeCount() const = 0;

        /// CPU-side BVH + triangle data backing this acceleration structure.
        /// Path tracer backends that don't consume the device's GPU buffers
        /// (the CPU backend traverses this directly; the Metal backend builds
        /// its own MTLAccelerationStructure) read source geometry through it.
        virtual const Blas *cpuBlas() const = 0;
    };
} // namespace tracey