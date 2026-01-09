#include "vulkan_compute_pipeline_compiler.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include "../cpu/cpu_shader_binding_table.hpp"
#include "../../../ray_tracing/shader_module/cpu/cpu_shader_module.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <shaderc/shaderc.hpp>
namespace tracey
{
    std::vector<uint32_t> compileVulkanComputeRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt)
    {
        const auto rayGenShader = sbt.rayGen();

        std::stringstream shaderTemplate;

        // Load prelude shader code
        std::filesystem::path preludePath = std::filesystem::path(__FILE__).parent_path() / "vulkan_compute_prelude.comp";
        std::ifstream preludeFile(preludePath);
        if (!preludeFile.is_open())
        {
            throw std::runtime_error("Failed to open Vulkan compute prelude file: " + preludePath.string());
        }
        shaderTemplate << preludeFile.rdbuf() << "\n";

        std::filesystem::path intersectPath = std::filesystem::path(__FILE__).parent_path() / "vulkan_compute_intersect.comp";
        std::ifstream intersectFile(intersectPath);
        if (!intersectFile.is_open())
        {
            throw std::runtime_error("Failed to open Vulkan compute intersect file: " + intersectPath.string());
        }

        shaderTemplate << intersectFile.rdbuf() << "\n";

        std::stringstream ss;

        int tlasCount = 0;
        bool sawTlas = false;

        const auto bindingStartOffset = 6;

        for (const auto &binding : layout.bindings())
        {
            if (sawTlas)
            {
                throw std::runtime_error("Layout limitation: TLAS must be the last descriptor binding (no bindings allowed after AccelerationStructure).");
            }

            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                ss << "layout(set = 0, binding = " << binding.index + bindingStartOffset << ", rgba8) uniform writeonly image2D " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Buffer:
                ss << "layout(std430, set = 0, binding = " << binding.index + bindingStartOffset << ") buffer " << "Buffer" << binding.index + bindingStartOffset << " {\n";
                for (const auto &field : binding.structure->fields())
                {
                    ss << "    " << field.type << " " << field.name;
                    if (field.isArray)
                    {
                        ss << "[";
                        if (field.elementCount > 0)
                        {
                            ss << field.elementCount;
                        }
                        ss << "]";
                    }
                    ss << ";\n";
                }
                ss << "} " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
            {
                ++tlasCount;
                if (tlasCount > 1)
                {
                    throw std::runtime_error("Layout limitation: only one TLAS (AccelerationStructure) is supported for the compute pipeline.");
                }

                // Mark that we've encountered TLAS; no further bindings are allowed (enforced at top of loop).
                sawTlas = true;

                break;
            }
            default:
                throw std::runtime_error("Unsupported descriptor type in Vulkan Compute pipeline compiler");
            }
        }

        if (tlasCount == 0)
        {
            throw std::runtime_error("Layout limitation: exactly one TLAS (AccelerationStructure) must be provided and it must be the last descriptor.");
        }

        for (const auto &payload : layout.payloads())
        {
            ss << "struct " << payload.structure.name() << " {\n";
            for (const auto &field : payload.structure.fields())
            {
                ss << "    " << field.type << " " << field.name << ";\n";
            }
            ss << "};\n";
        }

        // Create a struct containing all payloads
        ss << "struct RayPayloads {\n";
        for (const auto &payload : layout.payloads())
        {
            ss << "    " << payload.structure.name() << " " << payload.name << ";\n";
        }
        ss << "};\n";

        // Declare a global variable for ray payloads
        ss << "RayPayloads payloads;\n";
        // Create defines for easy access to individual payloads
        for (const auto &payload : layout.payloads())
        {
            ss << "#define " << payload.name << " payloads." << payload.name << "\n";
        }

        const std::string userShaderBindings = ss.str();
        ss = std::stringstream();

        for (size_t i = 0; i < sbt.hitModules().size(); ++i)
        {
            const auto hitModule = sbt.hitModules()[i];
            const auto cpuModule = dynamic_cast<const CpuShaderModule *>(hitModule);
            std::string userSource(cpuModule->source());
            // replace user entry point with rayGenMain
            size_t entryPointPos = userSource.find(cpuModule->entryPoint());
            if (entryPointPos != std::string_view::npos)
            {
                userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "HitShader" + std::to_string(i));
            }

            ss << userSource << "\n";
        }

        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(rayGenShader);

        std::string userSource(cpuModule->source());
        // replace user entry point with rayGenMain
        size_t entryPointPos = userSource.find(cpuModule->entryPoint());
        if (entryPointPos != std::string_view::npos)
        {
            userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "rayGenMain");
        }
        ss << userSource;

        std::stringstream missShaderCalls;
        for (size_t i = 0; i < sbt.missModules().size(); ++i)
        {
            const auto missModule = sbt.missModules()[i];
            const auto cpuModule = dynamic_cast<const CpuShaderModule *>(missModule);
            std::string userSource(cpuModule->source());
            // replace user entry point with MissShaderX
            size_t entryPointPos = userSource.find(cpuModule->entryPoint());
            if (entryPointPos != std::string_view::npos)
            {
                userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "MissShader" + std::to_string(i));
            }
            ss << userSource << "\n";
        }

        const std::string userShaderCode = ss.str();
        ss = std::stringstream();

        ss << "switch(missIndex)\n{\n";
        for (size_t i = 0; i < sbt.missModules().size(); ++i)
        {
            ss << "    case " << i << ":\n";
            ss << "        " << "MissShader" + std::to_string(i) + "();\n";
            ss << "        break;\n";
        }
        ss << "    default:\n";
        ss << "        break;\n";
        ss << "}\n";

        const std::string missShaderCallsCode = ss.str();
        ss = std::stringstream();

        ss << "switch(hitIndex)\n{\n";
        for (size_t i = 0; i < sbt.hitModules().size(); ++i)
        {
            ss << "    case " << i << ":\n";
            ss << "        " << "HitShader" + std::to_string(i) + "();\n";
            ss << "        break;\n";
        }
        ss << "    default:\n";
        ss << "        break;\n";
        ss << "}\n";

        const std::string hitShaderCallsCode = ss.str();

        std::string finalShader = shaderTemplate.str();
        // Replace placeholders
        size_t pos = finalShader.find("// [USER_SHADER_BINDINGS]");
        if (pos != std::string::npos)
        {
            finalShader.replace(pos, std::string("// [USER_SHADER_BINDINGS]").length(), userShaderBindings);
        }

        pos = finalShader.find("// [USER_SHADER_CODE]");
        if (pos != std::string::npos)
        {
            finalShader.replace(pos, std::string("// [USER_SHADER_CODE]").length(), userShaderCode);
        }

        pos = finalShader.find("// [MISS_SHADER_CALLS]");
        if (pos != std::string::npos)
        {
            finalShader.replace(pos, std::string("// [MISS_SHADER_CALLS]").length(), missShaderCallsCode);
        }

        pos = finalShader.find("// [HIT_SHADER_CALLS]");
        if (pos != std::string::npos)
        {
            finalShader.replace(pos, std::string("// [HIT_SHADER_CALLS]").length(), hitShaderCallsCode);
        }

        printf("Vulkan Compute Ray Tracing Shader Source:\n%s\n", finalShader.c_str());

        // Compile finalShader using shaderc
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(finalShader.c_str(), finalShader.size(), shaderc_compute_shader, "RayGenShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Pipeline compilation failed: " + module.GetErrorMessage());
        }

        std::vector<uint32_t> spirvCode(module.cbegin(), module.cend());
        return spirvCode;
    }
    std::vector<uint32_t> compileRayGenShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt)
    {
        std::stringstream ss;
        const auto rayGenShader = sbt.rayGen();
        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(rayGenShader);
        std::string userSource(cpuModule->source());
        // replace user entry point with rayGenMain
        size_t entryPointPos = userSource.find(cpuModule->entryPoint());
        if (entryPointPos != std::string_view::npos)
        {
            userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "ray_gen_main");
        }
        ss << userSource;

        printf("Vulkan Compute Ray Tracing Shader Source:\n%s\n", ss.str().c_str());
        // Compile finalShader using shaderc
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(ss.str().c_str(), ss.str().size(), shaderc_compute_shader, "RayGenShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Pipeline compilation failed: " + module.GetErrorMessage());
        }

        std::vector<uint32_t> spirvCode(module.cbegin(), module.cend());
        return spirvCode;
    }
}
