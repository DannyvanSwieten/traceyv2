#include "cpu_pipeline_compiler.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include "../../../device/cpu/cpu_top_level_acceleration_structure.hpp"
#include "../../shader_module/cpu/cpu_shader_module.hpp"
#include "../../../core/tlas.hpp"
#include "cpu_shader_binding_table.hpp"
#include "trace_rays_ext.hpp"
#include <sstream>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <dlfcn.h>
#include <cassert>

namespace tracey
{
    constexpr const std::string_view TLAS_TYPE_NAME = "accelerationStructureEXT";
    constexpr const std::string_view IMAGE2D_TYPE_NAME = "image2d";
    constexpr const std::string_view BUFFER_TYPE_NAME = "buffer";

    CompiledShader compileCpuRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt)
    {
        const auto rayGenModule = dynamic_cast<const CpuShaderModule *>(sbt.rayGen());
        if (!rayGenModule)
        {
            throw std::runtime_error("Invalid ray generation shader module in SBT.");
        }

        Sbt compiledSbt{RayGenShader(compileCpuShader(layout, rayGenModule->stage(), rayGenModule->source(), rayGenModule->entryPoint()))};
        for (const auto &hitModulePtr : sbt.hitModules())
        {
            const auto hitModule = dynamic_cast<const CpuShaderModule *>(hitModulePtr);
            if (!hitModule)
            {
                throw std::runtime_error("Invalid shader module in SBT.");
            }

            compiledSbt.hits.push_back(compileCpuShader(layout, hitModule->stage(), hitModule->source(), hitModule->entryPoint()));
        }
        return CompiledShader();
    }
    CompiledShader compileCpuShader(const RayTracingPipelineLayout &layout, ShaderStage stage, const std::string_view source, const std::string_view entryPoint)
    {
        assert(!entryPoint.empty());
        std::string userSourceModified = std::string(source);
        std::stringstream shader;
        shader << "#include <runtime/rt_symbols.h>\n";
        shader << "using namespace rt;\n";
        shader << "extern \"C\" TraceRaysFunc_t traceRaysEXT = nullptr;" << "\n";
        shader << "extern \"C\" ImageStoreFunc_t imageStore = nullptr;\n";
        shader << "extern \"C\" thread_local Builtins g_Builtins = {};\n";
        shader << "extern \"C\" void setBuiltins(const Builtins &b) { g_Builtins = b; }\n";
        shader << "extern \"C\" void getBuiltins(Builtins* b) { *b = g_Builtins; }\n";

        const auto bindings = layout.bindingsForStage(stage);
        for (const auto &binding : bindings)
        {
            switch (binding.type)
            {
            case RayTracingPipelineLayout::DescriptorType::Image2D:
                shader << "extern \"C\" " << IMAGE2D_TYPE_NAME << " " << binding.name << " = nullptr;\n";
                break;
            case RayTracingPipelineLayout::DescriptorType::Buffer:
            {
                if (binding.structure.has_value())
                {
                    shader << "struct " << binding.structure->name() << " {\n";
                    for (const auto &field : binding.structure->fields())
                    {
                        shader << "    " << field.type << " " << field.name;
                        if (field.isArray)
                        {
                            if (field.elementCount == 0)
                            {
                                shader << "[]";
                            }
                            else
                                shader << "[" << field.elementCount << "]";
                        }
                        shader << ";\n";
                    }
                    shader << "};\n";
                    shader << "extern \"C\" " << BUFFER_TYPE_NAME << " " << binding.name << " = nullptr;\n";
                    // Define a macro to access elements
                    shader << "#define " << binding.name << " reinterpret_cast<" << binding.structure->name() << "*>(" << binding.name << ")\n";
                }

                break;
            }
            case RayTracingPipelineLayout::DescriptorType::AccelerationStructure:
                shader << "extern \"C\" " << TLAS_TYPE_NAME << " " << binding.name << " = nullptr;\n";
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
        const auto newSource = shader.str();

        std::cout << "Generated CPU Shader Source:\n"
                  << newSource << std::endl;

        std::filesystem::path cppPath = std::filesystem::temp_directory_path() / "generated_kernel.cpp";
        using clock = std::chrono::high_resolution_clock;
        auto now = clock::now().time_since_epoch().count();
        std::filesystem::path outputPath = std::filesystem::temp_directory_path() / ("generated_kernel_" + std::to_string(now) + ".dylib");
        std::filesystem::path includePath = std::filesystem::current_path() / "src" / "ray_tracing" / "ray_tracing_pipeline" / "cpu";
        std::ofstream outFile(cppPath);
        outFile << newSource;
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

        const auto entryPointFunc = reinterpret_cast<RayTracingEntryPointFunc>(dlsym(dylib, entryPoint.data()));
        if (!entryPointFunc)
        {
            std::cerr << "dlopen error: " << dlerror() << std::endl;
            dlclose(dylib);
            std::filesystem::remove(outputPath);
            throw std::runtime_error("Failed to find entry point function in dylib: " + std::string(dlerror()));
        }
        CompiledShader compiledShader;
        compiledShader.func = entryPointFunc;
        // load all global bindingslots
        for (const auto &binding : layout.bindingsForStage(stage))
        {
            void **slotPtr = reinterpret_cast<void **>(dlsym(dylib, binding.name.c_str()));
            if (!slotPtr)
            {
                std::cerr << "dlopen error: " << dlerror() << std::endl;
                dlclose(dylib);
                std::filesystem::remove(outputPath);
                throw std::runtime_error("Failed to find binding slot in dylib: " + std::string(dlerror()));
            }
            // Store the slot pointer somewhere to be set later during execution
            compiledShader.bindingSlots.push_back(BindingSlot{slotPtr});
        }

        compiledShader.dylib = dylib;

        return compiledShader;
    }
    void CompiledShader::setTraceRaysExt()
    {
        if (dylib)
        {
            rt::TraceRaysFunc_t *traceFuncPtr = reinterpret_cast<rt::TraceRaysFunc_t *>(dlsym(dylib, "traceRaysEXT"));
            if (traceFuncPtr)
            {
                *traceFuncPtr = reinterpret_cast<rt::TraceRaysFunc_t>(traceRaysExtFunc);
            }

            rt::ImageStoreFunc_t *imageStorePtr = reinterpret_cast<rt::ImageStoreFunc_t *>(dlsym(dylib, "imageStore"));
            if (imageStorePtr)
            {
                *imageStorePtr = reinterpret_cast<rt::ImageStoreFunc_t>(imageStoreFunc);
            }
        }
    }
    RayGenShader::RayGenShader(CompiledShader shader) : shader(shader)
    {
        this->setBuiltins = reinterpret_cast<setBuiltinsFunc>(dlsym(this->shader.dylib, "setBuiltins"));
        this->getBuiltins = reinterpret_cast<getBuiltinsFunc>(dlsym(this->shader.dylib, "getBuiltins"));
    }
    Sbt::Sbt(RayGenShader rayGenShader) : rayGen(rayGenShader), hits()
    {
        rayGen.shader.setTraceRaysExt();
    }
    ClosestHitShader::ClosestHitShader(CompiledShader shader) : shader(shader)
    {
        this->setBuiltins = reinterpret_cast<setBuiltinsFunc>(dlsym(this->shader.dylib, "setBuiltins"));
        this->getBuiltins = reinterpret_cast<getBuiltinsFunc>(dlsym(this->shader.dylib, "getBuiltins"));
    }
}