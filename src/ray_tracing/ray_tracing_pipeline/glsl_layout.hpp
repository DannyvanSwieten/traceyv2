// GLSL std140 / std430 layout math — the single source of truth shared
// by the host side (ShaderInputsBuffer computes member offsets when
// writing uniform data) and the shader side (the pipeline compiler emits
// padded struct declarations). Both must agree byte-for-byte or uniforms
// silently land on the wrong members.

#pragma once

#include "data_structure.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace tracey
{
    // Alignment / size per GLSL type name. std140 pads vec3 to 16 bytes;
    // std430 keeps the 12-byte size but the 16-byte alignment.
    size_t getStd140Alignment(const std::string &glslType);
    size_t getStd140Size(const std::string &glslType);
    size_t getStd430Alignment(const std::string &glslType);
    size_t getStd430Size(const std::string &glslType);

    // Emit `struct <name> { ... };` with explicit _padN members so the
    // GLSL compiler's offsets match the host-side arithmetic above.
    void generateStd140Struct(std::stringstream &ss, const std::string &structName,
                              const std::vector<StructField> &fields);
    void generateStd430Struct(std::stringstream &ss, const std::string &structName,
                              const std::vector<StructField> &fields);
}
