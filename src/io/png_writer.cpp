#include "png_writer.hpp"

// Declarations only — STB_IMAGE_WRITE_IMPLEMENTATION is defined once in
// gltf_loader.cpp, so stbi_write_png is an extern symbol we link against.
#include <stb_image_write.h>

namespace tracey
{
    bool writePng(const std::string &path, int width, int height, const uint8_t *rgba8)
    {
        if (path.empty() || width <= 0 || height <= 0 || !rgba8) return false;
        // 4 = RGBA; stride = width*4 (tightly packed rows).
        return stbi_write_png(path.c_str(), width, height, 4, rgba8, width * 4) != 0;
    }
}
