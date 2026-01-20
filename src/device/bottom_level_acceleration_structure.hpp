#pragma once
#include <cstddef>

namespace tracey
{
    class BottomLevelAccelerationStructure
    {
    public:
        virtual ~BottomLevelAccelerationStructure() = default;

        /// Get the number of BVH nodes (for statistics/debugging)
        virtual size_t nodeCount() const = 0;
    };
} // namespace tracey