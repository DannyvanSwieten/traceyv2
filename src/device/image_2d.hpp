#pragma once

namespace tracey
{
    class Image2D
    {
    public:
        virtual ~Image2D() = default;
        virtual char *data() = 0;
    };
} // namespace tracey