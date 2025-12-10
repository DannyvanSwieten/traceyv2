#pragma once
#include <cstdint>
namespace tracey
{
    class Image2D;
    class Buffer;
    class TopLevelAccelerationStructure;
    class DescriptorSet
    {
    public:
        virtual ~DescriptorSet() = default;
        virtual void setImage2D(uint32_t binding, Image2D *image) = 0;
        virtual void setBuffer(uint32_t binding, Buffer *buffer) = 0;
        virtual void setAccelerationStructure(uint32_t binding, const TopLevelAccelerationStructure *tlas) = 0;
    };
} // namespace tracey