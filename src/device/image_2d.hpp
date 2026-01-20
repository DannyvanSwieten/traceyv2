#pragma once
#include <cstdint>

namespace tracey
{
    class Image2D
    {
    public:
        virtual ~Image2D() = default;
        virtual char *data() = 0;
        virtual uint32_t width() const = 0;
        virtual uint32_t height() const = 0;
    };
} // namespace tracey