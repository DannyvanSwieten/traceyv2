#pragma once
#include <string_view>
namespace tracey
{
    class Image2D;
    class Buffer;
    class TopLevelAccelerationStructure;
    class DescriptorSet
    {
    public:
        virtual ~DescriptorSet() = default;
        virtual void setImage2D(const std::string_view name, Image2D *image) = 0;
        virtual void setBuffer(const std::string_view name, Buffer *buffer) = 0;
        virtual void setUniformBuffer(const std::string_view name, Buffer *buffer) = 0;
        virtual void setAccelerationStructure(const std::string_view name, const TopLevelAccelerationStructure *tlas) = 0;
    };
} // namespace tracey