#include "cpu_image_2d.hpp"
namespace tracey
{
    CpuImage2D::CpuImage2D(uint32_t width, uint32_t height, ImageFormat format) : m_width(width), m_height(height), m_format(format)
    {
        // Allocate memory for the image data based on width, height, and format
        switch (format)
        {
        case ImageFormat::R8G8B8A8Unorm:
            bytesPerPixel = 4; // 4 bytes per pixel
            break;
        case ImageFormat::R32G32B32A32Sfloat:
            bytesPerPixel = 16; // 16 bytes per pixel
            break;
        case ImageFormat::R32Sfloat:
            bytesPerPixel = 4; // 4 bytes per pixel
            break;
        default:
            throw std::runtime_error("Unsupported image format");
        }

        m_data = static_cast<char *>(std::malloc(width * height * bytesPerPixel));
        if (!m_data)
        {
            throw std::bad_alloc();
        }
    }

    CpuImage2D::~CpuImage2D()
    {
        // Free m_data if it was allocated
        if (m_data)
        {
            std::free(m_data);
            m_data = nullptr;
        }
    }
    void CpuImage2D::store(uint32_t x, uint32_t y, const Vec4 &value)
    {
        const auto pixelOffset = (y * m_width + x);
        char *pixelPtr = m_data + pixelOffset * bytesPerPixel;
        switch (m_format)
        {
        case ImageFormat::R8G8B8A8Unorm:
        {
            uint8_t r = static_cast<uint8_t>(glm::clamp(value.r * 255.0f, 0.0f, 255.0f));
            uint8_t g = static_cast<uint8_t>(glm::clamp(value.g * 255.0f, 0.0f, 255.0f));
            uint8_t b = static_cast<uint8_t>(glm::clamp(value.b * 255.0f, 0.0f, 255.0f));
            uint8_t a = static_cast<uint8_t>(glm::clamp(value.a * 255.0f, 0.0f, 255.0f));
            pixelPtr[0] = static_cast<char>(r);
            pixelPtr[1] = static_cast<char>(g);
            pixelPtr[2] = static_cast<char>(b);
            pixelPtr[3] = static_cast<char>(a);
            break;
        }
        case ImageFormat::R32G32B32A32Sfloat:
        {
            float *floatPtr = reinterpret_cast<float *>(pixelPtr);
            floatPtr[0] = value.r;
            floatPtr[1] = value.g;
            floatPtr[2] = value.b;
            floatPtr[3] = value.a;
            break;
        }
        case ImageFormat::R32Sfloat:
        {
            float *floatPtr = reinterpret_cast<float *>(pixelPtr);
            floatPtr[0] = value.r;
            break;
        }
        }
    }
}