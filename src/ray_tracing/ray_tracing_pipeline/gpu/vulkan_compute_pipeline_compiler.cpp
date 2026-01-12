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

        const auto bindingStartOffset = 5;

        for (const auto &binding : layout.bindings())
        {
            const auto bindingIndex = layout.indexForBinding(binding.name) + bindingStartOffset;
            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                ss << "layout(set = 0, binding = " << bindingIndex << ", rgba8) uniform writeonly image2D " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::StorageBuffer:
                ss << "layout(std430, set = 0, binding = " << bindingIndex << ") buffer " << "Buffer" << bindingIndex << " {\n";
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
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(finalShader.c_str(), finalShader.size(), shaderc_compute_shader, "RayGenShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Pipeline compilation failed: " + module.GetErrorMessage());
        }

        std::vector<uint32_t> spirvCode(module.cbegin(), module.cend());
        return spirvCode;
    }
    WaveFrontPipelineCompileResult compileVulkanWaveFrontRayTracingPipeline(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt)
    {
        const auto rayGenSpirV = compileRayGenShader(layout, sbt);
        std::vector<std::vector<uint32_t>> hitShadersSpirV;
        for (size_t i = 0; i < sbt.hitModules().size(); ++i)
        {
            hitShadersSpirV.push_back(compileHitShader(layout, sbt, i));
        }

        // std::vector<std::vector<uint32_t>> missShadersSpirV;
        // for (size_t i = 0; i < sbt.missModules().size(); ++i)
        // {
        //     missShadersSpirV.push_back(compileMissShader(layout, sbt, i));
        // }
        return WaveFrontPipelineCompileResult{std::move(rayGenSpirV), std::move(hitShadersSpirV)};
    }
    std::vector<uint32_t> compileRayGenShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt)
    {
        std::filesystem::path rayGenShaderPath = std::filesystem::path(__FILE__).parent_path() / "wavefront" / "vulkan_wavefront_ray_gen.comp";
        std::ifstream rayGenShaderFile(rayGenShaderPath);
        if (!rayGenShaderFile.is_open())
        {
            throw std::runtime_error("Failed to open Vulkan WaveFront ray generation shader file: " + rayGenShaderPath.string());
        }
        std::stringstream rayGenShaderTemplateStream;
        rayGenShaderTemplateStream << rayGenShaderFile.rdbuf() << "\n";
        std::string rayGenShaderTemplate = rayGenShaderTemplateStream.str();

        const auto rayGenShader = sbt.rayGen();
        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(rayGenShader);
        std::string userSource(cpuModule->source());
        // replace user entry point with rayGenMain
        size_t entryPointPos = userSource.find(cpuModule->entryPoint());
        if (entryPointPos != std::string_view::npos)
        {
            userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "ray_gen_main");
        }

        const auto userSourcePosition = rayGenShaderTemplate.find("//___RAY_GENERATION_FUNCTION___");
        if (userSourcePosition != std::string::npos)
        {
            rayGenShaderTemplate.replace(userSourcePosition, std::string("//___RAY_GENERATION_FUNCTION___").length(), userSource);
        }

        printf("Vulkan Compute Ray Tracing Shader Source:\n%s\n", rayGenShaderTemplate.c_str());
        // Compile finalShader using shaderc
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(rayGenShaderTemplate.c_str(), rayGenShaderTemplate.size(), shaderc_compute_shader, "RayGenShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Pipeline compilation failed: " + module.GetErrorMessage());
        }

        std::vector<uint32_t> spirvCode(module.cbegin(), module.cend());
        return spirvCode;
    }

    std::vector<uint32_t> compileHitShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt, size_t hitShaderIndex)
    {
        std::filesystem::path hitShaderPath = std::filesystem::path(__FILE__).parent_path() / "wavefront" / "vulkan_wavefront_hit_shader.comp";
        std::ifstream hitShaderFile(hitShaderPath);
        if (!hitShaderFile.is_open())
        {
            throw std::runtime_error("Failed to open Vulkan WaveFront hit shader file: " + hitShaderPath.string());
        }
        std::stringstream hitShaderTemplateStream;
        hitShaderTemplateStream << hitShaderFile.rdbuf() << "\n";
        std::string hitShaderTemplate = hitShaderTemplateStream.str();

        const auto hitShader = sbt.hitModules()[hitShaderIndex];
        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(hitShader);
        std::string userSource(cpuModule->source());
        // replace user entry point with hitShaderX
        size_t entryPointPos = userSource.find(cpuModule->entryPoint());
        if (entryPointPos != std::string_view::npos)
        {
            userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "hit_shader_main");
        }

        const auto userSourcePosition = hitShaderTemplate.find("//___HIT_SHADER_FUNCTION___");
        if (userSourcePosition != std::string::npos)
        {
            hitShaderTemplate.replace(userSourcePosition, std::string("//___HIT_SHADER_FUNCTION___").length(), userSource);
        }

        printf("Vulkan Compute Ray Tracing Hit Shader Source:\n%s\n", hitShaderTemplate.c_str());
        // Compile finalShader using shaderc
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(hitShaderTemplate.c_str(), hitShaderTemplate.size(), shaderc_compute_shader, "HitShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Hit Shader compilation failed: " + module.GetErrorMessage());
        }

        std::vector<uint32_t> spirvCode(module.cbegin(), module.cend());
        return spirvCode;
    }
    std::vector<uint32_t> compileMissShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt, size_t missShaderIndex)
    {
        std::stringstream ss;
        const auto missShader = sbt.missModules()[missShaderIndex];
        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(missShader);
        std::string userSource(cpuModule->source());
        // replace user entry point with missShaderX
        size_t entryPointPos = userSource.find(cpuModule->entryPoint());
        if (entryPointPos != std::string_view::npos)
        {
            userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "miss_shader_" + std::to_string(missShaderIndex));
        }
        ss << userSource;

        printf("Vulkan Compute Ray Tracing Miss Shader Source:\n%s\n", ss.str().c_str());
        // Compile finalShader using shaderc
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(ss.str().c_str(), ss.str().size(), shaderc_compute_shader, "MissShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Miss Shader compilation failed: " + module.GetErrorMessage());
        }

        std::vector<uint32_t> spirvCode(module.cbegin(), module.cend());
        return spirvCode;
    }
}
