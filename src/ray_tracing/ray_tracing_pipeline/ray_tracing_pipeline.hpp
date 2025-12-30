#pragma once
#include <span>
#include "descriptor_set.hpp"
namespace tracey
{
    class RayTracingPipeline
    {
    public:
        virtual ~RayTracingPipeline() = default;

        virtual void allocateDescriptorSets(std::span<DescriptorSet *> sets) = 0;
    };
}