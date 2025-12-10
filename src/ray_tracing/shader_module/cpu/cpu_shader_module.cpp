#include "cpu_shader_module.hpp"
#include "../../ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include <sstream>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <dlfcn.h>
tracey::CpuShaderModule::CpuShaderModule(const RayTracingPipelineLayout &layout, ShaderStage stage, const std::string_view source, const std::string_view entryPoint)
{
    std::string userSourceModified = std::string(source);
    std::stringstream shader;
    shader << "#include <rt_symbols.h>\n";
    shader << "using namespace rt;\n";
    shader << "\n";

    const auto bindings = layout.bindingsForStage(stage);
    for (const auto &binding : bindings)
    {
        switch (binding.type)
        {
        case RayTracingPipelineLayout::DescriptorType::Image2D:
            shader << "extern \"C\" Image2D " << binding.name << ";\n";
            break;
        case RayTracingPipelineLayout::DescriptorType::Buffer:
            shader << "extern \"C\" Buffer " << binding.name << ";\n";
            break;
        case RayTracingPipelineLayout::DescriptorType::AccelerationStructure:
            shader << "extern \"C\" accelerationStructureEXT " << binding.name << ";\n";
            break;
        default:
            break;
        }
    }

    std::string toReplace = "void " + std::string(entryPoint) + "()";
    std::string replacement = "extern \"C\" void " + std::string(entryPoint) + "()";
    size_t pos = userSourceModified.find(toReplace);
    if (pos != std::string::npos)
    {
        userSourceModified.replace(pos, toReplace.length(), replacement);
    }

    shader << userSourceModified << "\n";
    m_source = shader.str();

    std::cout << "Generated CPU Shader Source:\n"
              << m_source << std::endl;

    std::filesystem::path cppPath = std::filesystem::temp_directory_path() / "generated_kernel.cpp";
    using clock = std::chrono::high_resolution_clock;
    auto now = clock::now().time_since_epoch().count();
    std::filesystem::path outputPath = std::filesystem::temp_directory_path() / ("generated_kernel_" + std::to_string(now) + ".dylib");
    std::filesystem::path includePath = std::filesystem::current_path() / "src" / "ray_tracing" / "ray_tracing_pipeline" / "cpu";
    std::ofstream outFile(cppPath);
    outFile << m_source;
    outFile.close();

    // Compile it to a dylib and import it again...
    std::stringstream cmdStream;
    cmdStream << "clang++ -std=c++20 -O3 -shared -fPIC "
              << "-I" << includePath << " "
              << cppPath.string() << " -o "
              << outputPath.string();

    int result = std::system(cmdStream.str().c_str());
    if (result != 0)
    {
        // handle compile error (you can capture stderr if you want)
        throw std::runtime_error("Failed to compile generated shader code.");
    }

    // Delete the temporary source file
    std::filesystem::remove(cppPath);

    // Load the dylib and get function pointers as needed...
    const auto dylib = dlopen(outputPath.string().c_str(), RTLD_NOW);
    if (!dylib)
    {
        // Check the error
        std::cerr << "dlopen error: " << dlerror() << std::endl;
        std::filesystem::remove(outputPath);
        throw std::runtime_error("Failed to load compiled shader dylib: " + std::string(dlerror()));
    }

    using MainFuncType = void (*)();
    m_entryPointFunc = reinterpret_cast<MainFuncType>(dlsym(dylib, entryPoint.data()));
    if (!m_entryPointFunc)
    {
        std::cerr << "dlopen error: " << dlerror() << std::endl;
        dlclose(dylib);
        std::filesystem::remove(outputPath);
        throw std::runtime_error("Failed to find entry point function in dylib: " + std::string(dlerror()));
    }

    m_dylibHandle = dylib;
}