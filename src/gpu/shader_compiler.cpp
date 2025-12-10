#include "shader_compiler.hpp"
#include <shaderc/shaderc.hpp>
#include <fstream>

namespace tracey
{
    std::vector<uint32_t> ShaderCompiler::compileComputeShader(const std::string_view source, const std::string_view sourceName)
    {
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;

        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(source.data(), source.size(), shaderc_compute_shader, sourceName.data(), options);

        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Shader compilation failed: " + module.GetErrorMessage());
        }

        return {module.cbegin(), module.cend()};
    }
    std::vector<uint32_t> ShaderCompiler::compileComputeShader(const std::filesystem::path &filePath)
    {
        std::ifstream file(filePath);
        if (!file)
        {
            throw std::runtime_error("Failed to open shader file: " + filePath.string());
        }

        std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        return compileComputeShader(source, filePath.filename().string());
    }
}