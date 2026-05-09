#pragma once

#include <filesystem>
#include <string>

namespace tracey
{
    // Resolve `#include "..."` directives recursively, relative to basePath.
    // Throws std::runtime_error on circular includes (depth > 32) or missing
    // files. Other GLSL constructs (define, version, layout, ...) are passed
    // through untouched -- this is a simple string-level resolver, not a
    // full preprocessor.
    std::string preprocessGlsl(const std::string &source,
                               const std::filesystem::path &basePath);
}
