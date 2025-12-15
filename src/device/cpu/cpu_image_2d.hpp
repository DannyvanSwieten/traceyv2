#pragma once
#include "../image_2d.hpp"
#include "../device.hpp"
namespace tracey
{
    class CpuImage2D : public Image2D
    {
    public:
        CpuImage2D(uint32_t width, uint32_t height, ImageFormat format);
        ~CpuImage2D();

        void store(uint32_t x, uint32_t y, const Vec4 &value);
        char *data() { return m_data; }

    private:
        char *m_data;
        ImageFormat m_format;
        uint32_t m_width;
        uint32_t m_height;
        uint32_t bytesPerPixel;
    };
} // namespace tracey