#pragma once
#include <cstdint>
#include <string>

namespace tracey
{
    // Write an 8-bit RGBA image to a PNG file. `rgba8` is width*height*4 bytes,
    // row-major, top-down, and already display-ready (tonemapped + gamma —
    // i.e. the path tracer's LDR readback). Returns true on success. Backed by
    // stb_image_write (its implementation is already compiled into the engine
    // via gltf_loader.cpp).
    bool writePng(const std::string &path, int width, int height, const uint8_t *rgba8);
}
