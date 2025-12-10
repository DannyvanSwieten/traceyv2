#pragma once
#include <string_view>
#include <vector>
#include <filesystem>
namespace tracey
{
    class ShaderCompiler
    {
    public:
        std::vector<uint32_t> compileComputeShader(const std::filesystem::path &filePath);
        std::vector<uint32_t> compileComputeShader(const std::string_view source, const std::string_view name);
    };
}