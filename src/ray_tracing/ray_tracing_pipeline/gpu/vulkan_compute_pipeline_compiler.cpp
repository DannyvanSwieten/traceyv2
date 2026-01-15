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
    // Helper: Get alignment requirement for GLSL type in std430 layout
    static size_t getStd430Alignment(const std::string &glslType)
    {
        if (glslType == "float" || glslType == "int" || glslType == "uint")
            return 4;
        if (glslType == "vec2" || glslType == "ivec2" || glslType == "uvec2")
            return 8;
        if (glslType == "vec3" || glslType == "ivec3" || glslType == "uvec3")
            return 16; // std430: vec3 is 16-byte aligned
        if (glslType == "vec4" || glslType == "ivec4" || glslType == "uvec4")
            return 16;
        if (glslType == "mat3")
            return 16; // mat3 is array of 3 vec3, each aligned to 16
        if (glslType == "mat4")
            return 16;
        return 16; // Default for unknown types
    }

    // Helper: Get size for GLSL type in std430 layout
    static size_t getStd430Size(const std::string &glslType)
    {
        if (glslType == "float" || glslType == "int" || glslType == "uint")
            return 4;
        if (glslType == "vec2" || glslType == "ivec2" || glslType == "uvec2")
            return 8;
        if (glslType == "vec3" || glslType == "ivec3" || glslType == "uvec3")
            return 12; // std430: vec3 is 12 bytes (but 16-byte aligned)
        if (glslType == "vec4" || glslType == "ivec4" || glslType == "uvec4")
            return 16;
        if (glslType == "mat3")
            return 48; // 3 * 16 (each column is vec3, 16-byte aligned)
        if (glslType == "mat4")
            return 64; // 4 * 16
        return 16;     // Default for unknown types
    }

    // Helper: Generate struct with proper std430 padding
    static void generateStd430Struct(std::stringstream &ss, const std::string &structName, const std::vector<StructField> &fields)
    {
        ss << "struct " << structName << " {\n";

        size_t currentOffset = 0;
        int paddingCounter = 0;

        for (const auto &field : fields)
        {
            size_t alignment = getStd430Alignment(field.type);
            size_t fieldSize = getStd430Size(field.type);

            // Calculate aligned offset
            size_t alignedOffset = (currentOffset + alignment - 1) & ~(alignment - 1);

            // Insert padding if needed
            if (alignedOffset > currentOffset)
            {
                size_t paddingSize = alignedOffset - currentOffset;
                // Determine padding type based on size
                if (paddingSize == 4)
                {
                    ss << "    float _pad" << paddingCounter++ << ";\n";
                }
                else if (paddingSize == 8)
                {
                    ss << "    vec2 _pad" << paddingCounter++ << ";\n";
                }
                else if (paddingSize == 12)
                {
                    ss << "    vec3 _pad" << paddingCounter++ << ";\n";
                }
                else if (paddingSize >= 16)
                {
                    // Use multiple vec4s for large padding
                    while (paddingSize >= 16)
                    {
                        ss << "    vec4 _pad" << paddingCounter++ << ";\n";
                        paddingSize -= 16;
                    }
                    if (paddingSize == 12)
                    {
                        ss << "    vec3 _pad" << paddingCounter++ << ";\n";
                    }
                    else if (paddingSize == 8)
                    {
                        ss << "    vec2 _pad" << paddingCounter++ << ";\n";
                    }
                    else if (paddingSize == 4)
                    {
                        ss << "    float _pad" << paddingCounter++ << ";\n";
                    }
                }
            }

            // Write the actual field
            ss << "    " << field.type << " " << field.name;
            if (field.isArray)
            {
                ss << "[";
                if (field.elementCount > 0)
                {
                    ss << field.elementCount;
                }
                ss << "]";
                fieldSize *= field.elementCount > 0 ? field.elementCount : 1;
            }
            ss << ";\n";

            currentOffset = alignedOffset + fieldSize;
        }

        // Align struct size to 16 bytes (std430 requirement for structs in arrays)
        size_t alignedSize = (currentOffset + 15) & ~15;
        if (alignedSize > currentOffset)
        {
            size_t finalPadding = alignedSize - currentOffset;
            if (finalPadding == 4)
            {
                ss << "    float _pad" << paddingCounter++ << ";\n";
            }
            else if (finalPadding == 8)
            {
                ss << "    vec2 _pad" << paddingCounter++ << ";\n";
            }
            else if (finalPadding == 12)
            {
                ss << "    vec3 _pad" << paddingCounter++ << ";\n";
            }
        }

        ss << "};\n";
    }
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

        // Compile intersection shader (standalone, no user code injection)
        std::filesystem::path intersectShaderPath = std::filesystem::path(__FILE__).parent_path() / "wavefront" / "vulkan_wavefront_intersect.comp";
        std::ifstream intersectShaderFile(intersectShaderPath);
        if (!intersectShaderFile.is_open())
        {
            throw std::runtime_error("Failed to open Vulkan WaveFront intersection shader file: " + intersectShaderPath.string());
        }
        std::stringstream intersectShaderStream;
        intersectShaderStream << intersectShaderFile.rdbuf();
        std::string intersectShaderSource = intersectShaderStream.str();

        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);

        shaderc::SpvCompilationResult intersectModule = compiler.CompileGlslToSpv(
            intersectShaderSource, shaderc_glsl_compute_shader, "vulkan_wavefront_intersect.comp", options);

        if (intersectModule.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Failed to compile intersection shader: " + std::string(intersectModule.GetErrorMessage()));
        }
        std::vector<uint32_t> intersectSpirV(intersectModule.cbegin(), intersectModule.cend());

        std::vector<std::vector<uint32_t>> hitShadersSpirV;
        for (size_t i = 0; i < sbt.hitModules().size(); ++i)
        {
            hitShadersSpirV.push_back(compileHitShader(layout, sbt, i));
        }

        std::vector<std::vector<uint32_t>> missShadersSpirV;
        for (size_t i = 0; i < sbt.missModules().size(); ++i)
        {
            missShadersSpirV.push_back(compileMissShader(layout, sbt, i));
        }

        const auto resolveSpirV = compileResolveShader(layout, sbt);

        return WaveFrontPipelineCompileResult{
            .rayGenShaderSpirV = std::move(rayGenSpirV),
            .intersectShaderSpirV = std::move(intersectSpirV),
            .hitShadersSpirV = std::move(hitShadersSpirV),
            .missShadersSpirV = std::move(missShadersSpirV),
            .resolveShaderSpirV = std::move(resolveSpirV)};
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

        // Generate user parameters (bindings and payloads)
        std::stringstream userParams;

        // User bindings start after TLAS (0-5) at binding 6
        const auto bindingStartOffset = 6;
        for (const auto &binding : layout.bindings())
        {
            const auto bindingIndex = layout.indexForBinding(binding.name) + bindingStartOffset;
            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                userParams << "layout(set = 0, binding = " << bindingIndex << ", rgba8) uniform writeonly image2D " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::StorageBuffer:
                userParams << "layout(std430, set = 0, binding = " << bindingIndex << ") buffer " << "Buffer" << bindingIndex << " {\n";
                for (const auto &field : binding.structure->fields())
                {
                    userParams << "    " << field.type << " " << field.name;
                    if (field.isArray)
                    {
                        userParams << "[";
                        if (field.elementCount > 0)
                        {
                            userParams << field.elementCount;
                        }
                        userParams << "]";
                    }
                    userParams << ";\n";
                }
                userParams << "} " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
                // TLAS bindings already defined in template
                break;
            default:
                break;
            }
        }

        // Generate payload structures with proper std430 padding
        for (const auto &payload : layout.payloads())
        {
            generateStd430Struct(userParams, payload.structure.name(), payload.structure.fields());
        }

        // Create payload container struct
        // Note: Individual payload structs are already properly aligned,
        // and struct members in std430 are naturally aligned to their size
        userParams << "struct RayPayloads {\n";
        for (const auto &payload : layout.payloads())
        {
            userParams << "    " << payload.structure.name() << " " << payload.name << ";\n";
        }
        userParams << "};\n";

        // Create a buffer for ray payloads at binding 20
        userParams << "layout(std430, set = 0, binding = " << 20 << ") buffer RayPayloadBuffer {\n";
        userParams << "    RayPayloads payloads[];\n";
        userParams << "} rayPayloadBuffer;\n";

        // Inject user parameters
        const auto userParamsPosition = rayGenShaderTemplate.find("//___USER_PARAMS___");
        if (userParamsPosition != std::string::npos)
        {
            rayGenShaderTemplate.replace(userParamsPosition, std::string("//___USER_PARAMS___").length(), userParams.str());
        }

        // Generate function signature with payload parameter
        std::stringstream rayGenSignature;
        rayGenSignature << "void ray_gen_main(uvec2 pixel, inout RayPayloads payload);\n";

        const auto signaturePosition = rayGenShaderTemplate.find("//___RAY_GEN_SIGNATURE___");
        if (signaturePosition != std::string::npos)
        {
            rayGenShaderTemplate.replace(signaturePosition, std::string("//___RAY_GEN_SIGNATURE___").length(), rayGenSignature.str());
        }

        // Generate function call with payload reference
        std::stringstream rayGenCall;
        rayGenCall << "ray_gen_main(pixelCoord, rayPayloadBuffer.payloads[index]);\n";

        const auto callPosition = rayGenShaderTemplate.find("//___RAY_GEN_CALL___");
        if (callPosition != std::string::npos)
        {
            rayGenShaderTemplate.replace(callPosition, std::string("//___RAY_GEN_CALL___").length(), rayGenCall.str());
        }

        const auto rayGenShader = sbt.rayGen();
        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(rayGenShader);
        std::string userSource(cpuModule->source());
        // replace user entry point with ray_gen_main
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

        // Generate user parameters (bindings and payloads)
        std::stringstream userParams;

        // User bindings start after TLAS (0-5) at binding 6
        const auto bindingStartOffset = 6;
        for (const auto &binding : layout.bindings())
        {
            const auto bindingIndex = layout.indexForBinding(binding.name) + bindingStartOffset;
            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                userParams << "layout(set = 0, binding = " << bindingIndex << ", rgba8) uniform writeonly image2D " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::StorageBuffer:
                userParams << "layout(std430, set = 0, binding = " << bindingIndex << ") buffer " << "Buffer" << bindingIndex << " {\n";
                for (const auto &field : binding.structure->fields())
                {
                    userParams << "    " << field.type << " " << field.name;
                    if (field.isArray)
                    {
                        userParams << "[";
                        if (field.elementCount > 0)
                        {
                            userParams << field.elementCount;
                        }
                        userParams << "]";
                    }
                    userParams << ";\n";
                }
                userParams << "} " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
                // TLAS bindings already defined in template
                break;
            default:
                break;
            }
        }

        // Generate payload structures with proper std430 padding
        for (const auto &payload : layout.payloads())
        {
            generateStd430Struct(userParams, payload.structure.name(), payload.structure.fields());
        }

        // Create payload container struct
        // Note: Individual payload structs are already properly aligned,
        // and struct members in std430 are naturally aligned to their size
        userParams << "struct RayPayloads {\n";
        for (const auto &payload : layout.payloads())
        {
            userParams << "    " << payload.structure.name() << " " << payload.name << ";\n";
        }
        userParams << "};\n";

        // Create a buffer for ray payloads at binding 20
        userParams << "layout(std430, set = 0, binding = " << 20 << ") buffer RayPayloadBuffer {\n";
        userParams << "    RayPayloads payloads[];\n";
        userParams << "} rayPayloadBuffer;\n";

        // Inject user parameters
        const auto userParamsPosition = hitShaderTemplate.find("//___USER_PARAMS___");
        if (userParamsPosition != std::string::npos)
        {
            hitShaderTemplate.replace(userParamsPosition, std::string("//___USER_PARAMS___").length(), userParams.str());
        }

        // Generate function signature with HitInfo and payload parameters
        std::stringstream hitShaderSignature;
        hitShaderSignature << "void hit_shader_main(HitInfo hitInfo, inout RayPayloads payload);\n";

        const auto signaturePosition = hitShaderTemplate.find("//___HIT_SHADER_SIGNATURE___");
        if (signaturePosition != std::string::npos)
        {
            hitShaderTemplate.replace(signaturePosition, std::string("//___HIT_SHADER_SIGNATURE___").length(), hitShaderSignature.str());
        }

        // Generate function call with HitInfo and payload reference
        std::stringstream hitShaderCall;
        hitShaderCall << "hit_shader_main(hitInfos.hits[rayIndex], rayPayloadBuffer.payloads[rayIndex]);\n";

        const auto callPosition = hitShaderTemplate.find("//___HIT_SHADER_CALL___");
        if (callPosition != std::string::npos)
        {
            hitShaderTemplate.replace(callPosition, std::string("//___HIT_SHADER_CALL___").length(), hitShaderCall.str());
        }

        const auto hitShader = sbt.hitModules()[hitShaderIndex];
        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(hitShader);
        std::string userSource(cpuModule->source());
        // replace user entry point with hit_shader_main
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
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
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
        std::filesystem::path missShaderPath = std::filesystem::path(__FILE__).parent_path() / "wavefront" / "vulkan_wavefront_miss_shader.comp";
        std::ifstream missShaderFile(missShaderPath);
        if (!missShaderFile.is_open())
        {
            throw std::runtime_error("Failed to open Vulkan WaveFront miss shader file: " + missShaderPath.string());
        }
        std::stringstream missShaderTemplateStream;
        missShaderTemplateStream << missShaderFile.rdbuf() << "\n";
        std::string missShaderTemplate = missShaderTemplateStream.str();

        // Generate user parameters (bindings and payloads)
        std::stringstream userParams;

        // User bindings start after TLAS (0-5) at binding 6
        const auto bindingStartOffset = 6;
        for (const auto &binding : layout.bindings())
        {
            const auto bindingIndex = layout.indexForBinding(binding.name) + bindingStartOffset;
            switch (binding.type)
            {
            case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                userParams << "layout(set = 0, binding = " << bindingIndex << ", rgba8) uniform writeonly image2D " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::StorageBuffer:
                userParams << "layout(std430, set = 0, binding = " << bindingIndex << ") buffer " << "Buffer" << bindingIndex << " {\n";
                for (const auto &field : binding.structure->fields())
                {
                    userParams << "    " << field.type << " " << field.name;
                    if (field.isArray)
                    {
                        userParams << "[";
                        if (field.elementCount > 0)
                        {
                            userParams << field.elementCount;
                        }
                        userParams << "]";
                    }
                    userParams << ";\n";
                }
                userParams << "} " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
                // TLAS bindings already defined in template
                break;
            default:
                break;
            }
        }

        // Generate payload structures with proper std430 padding
        for (const auto &payload : layout.payloads())
        {
            generateStd430Struct(userParams, payload.structure.name(), payload.structure.fields());
        }

        // Create payload container struct
        // Note: Individual payload structs are already properly aligned,
        // and struct members in std430 are naturally aligned to their size
        userParams << "struct RayPayloads {\n";
        for (const auto &payload : layout.payloads())
        {
            userParams << "    " << payload.structure.name() << " " << payload.name << ";\n";
        }
        userParams << "};\n";

        // Create a buffer for ray payloads at binding 20
        userParams << "layout(std430, set = 0, binding = " << 20 << ") buffer RayPayloadBuffer {\n";
        userParams << "    RayPayloads payloads[];\n";
        userParams << "} rayPayloadBuffer;\n";

        // Inject user parameters
        const auto userParamsPosition = missShaderTemplate.find("//___USER_PARAMS___");
        if (userParamsPosition != std::string::npos)
        {
            missShaderTemplate.replace(userParamsPosition, std::string("//___USER_PARAMS___").length(), userParams.str());
        }

        // Generate function signature with payload parameter
        std::stringstream missShaderSignature;
        missShaderSignature << "void miss_shader_main(inout RayPayloads payload);\n";

        const auto signaturePosition = missShaderTemplate.find("//___MISS_SHADER_SIGNATURE___");
        if (signaturePosition != std::string::npos)
        {
            missShaderTemplate.replace(signaturePosition, std::string("//___MISS_SHADER_SIGNATURE___").length(), missShaderSignature.str());
        }

        // Generate function call with payload reference
        std::stringstream missShaderCall;
        missShaderCall << "miss_shader_main(rayPayloadBuffer.payloads[rayIndex]);\n";

        const auto callPosition = missShaderTemplate.find("//___MISS_SHADER_CALL___");
        if (callPosition != std::string::npos)
        {
            missShaderTemplate.replace(callPosition, std::string("//___MISS_SHADER_CALL___").length(), missShaderCall.str());
        }

        const auto missShader = sbt.missModules()[missShaderIndex];
        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(missShader);
        std::string userSource(cpuModule->source());
        // replace user entry point with miss_shader_main
        size_t entryPointPos = userSource.find(cpuModule->entryPoint());
        if (entryPointPos != std::string_view::npos)
        {
            userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "miss_shader_main");
        }

        const auto userSourcePosition = missShaderTemplate.find("//___MISS_SHADER_FUNCTION___");
        if (userSourcePosition != std::string::npos)
        {
            missShaderTemplate.replace(userSourcePosition, std::string("//___MISS_SHADER_FUNCTION___").length(), userSource);
        }

        printf("Vulkan Compute Ray Tracing Miss Shader Source:\n%s\n", missShaderTemplate.c_str());
        // Compile finalShader using shaderc
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(missShaderTemplate.c_str(), missShaderTemplate.size(), shaderc_compute_shader, "MissShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Miss Shader compilation failed: " + module.GetErrorMessage());
        }

        std::vector<uint32_t> spirvCode(module.cbegin(), module.cend());
        return spirvCode;
    }

    std::vector<uint32_t> compileResolveShader(const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt)
    {
        std::filesystem::path resolveShaderPath = std::filesystem::path(__FILE__).parent_path() / "wavefront" / "vulkan_wavefront_resolve_shader.comp";
        std::ifstream resolveShaderFile(resolveShaderPath);
        if (!resolveShaderFile.is_open())
        {
            throw std::runtime_error("Failed to open Vulkan WaveFront resolve shader file: " + resolveShaderPath.string());
        }
        std::stringstream resolveShaderStream;
        resolveShaderStream << resolveShaderFile.rdbuf();
        std::string resolveShaderTemplate = resolveShaderStream.str();

        // Check if user provided a resolve shader
        const auto resolveShader = sbt.resolveShader();

        if (resolveShader != nullptr)
        {
            // User provided a custom resolve shader - inject it
            const auto cpuModule = dynamic_cast<const CpuShaderModule *>(resolveShader);

            // Inject user parameters (payload buffer + user bindings)
            std::stringstream userParams;

            // Generate payload structures with proper std430 padding
            for (const auto &payload : layout.payloads())
            {
                generateStd430Struct(userParams, payload.structure.name(), payload.structure.fields());
            }

            // Create payload container struct
            userParams << "struct RayPayloads {\n";
            for (const auto &payload : layout.payloads())
            {
                userParams << "    " << payload.structure.name() << " " << payload.name << ";\n";
            }
            userParams << "};\n";

            // Create a buffer for ray payloads at binding 20
            userParams << "layout(std430, set = 0, binding = " << 20 << ") readonly buffer RayPayloadBuffer {\n";
            userParams << "    RayPayloads payloads[];\n";
            userParams << "} rayPayloadBuffer;\n";

            // Add user-defined image/buffer bindings (starting at offset 6)
            const size_t bindingStartOffset = 6;
            for (const auto &binding : layout.bindings())
            {
                const size_t bindingIndex = layout.indexForBinding(binding.name);
                switch (binding.type)
                {
                case RayTracingPipelineLayoutDescriptor::DescriptorType::Image2D:
                    userParams << "layout(set = 0, binding = " << (bindingIndex + bindingStartOffset) << ", rgba8) uniform image2D " << binding.name << ";\n";
                    break;
                case RayTracingPipelineLayoutDescriptor::DescriptorType::StorageBuffer:
                    userParams << "layout(std430, set = 0, binding = " << (bindingIndex + bindingStartOffset) << ") buffer " << binding.name << "_buffer {\n";
                    userParams << "    float data[];\n";
                    userParams << "} " << binding.name << ";\n";
                    break;
                case RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure:
                    // TLAS not needed in resolve shader
                    break;
                case RayTracingPipelineLayoutDescriptor::DescriptorType::RayPayload:
                    // Payload is already handled above
                    break;
                }
            }

            // Inject user parameters
            const auto userParamsPosition = resolveShaderTemplate.find("//___USER_PARAMS___");
            if (userParamsPosition != std::string::npos)
            {
                resolveShaderTemplate.replace(userParamsPosition, std::string("//___USER_PARAMS___").length(), userParams.str());
            }

            // Generate function signature with pixel coord and payload parameters
            std::stringstream resolveShaderSignature;
            resolveShaderSignature << "void resolve_shader_main(uvec2 pixel, in RayPayloads payload);\n";

            const auto signaturePosition = resolveShaderTemplate.find("//___RESOLVE_SHADER_SIGNATURE___");
            if (signaturePosition != std::string::npos)
            {
                resolveShaderTemplate.replace(signaturePosition, std::string("//___RESOLVE_SHADER_SIGNATURE___").length(), resolveShaderSignature.str());
            }

            // Get user resolve shader code
            std::string userSource(cpuModule->source());

            // Replace user entry point with resolve_shader_main
            size_t entryPointPos = userSource.find(cpuModule->entryPoint());
            if (entryPointPos != std::string_view::npos)
            {
                userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "resolve_shader_main");
            }

            const auto userSourcePosition = resolveShaderTemplate.find("//___RESOLVE_SHADER_FUNCTION___");
            if (userSourcePosition != std::string::npos)
            {
                resolveShaderTemplate.replace(userSourcePosition, std::string("//___RESOLVE_SHADER_FUNCTION___").length(), userSource);
            }

            // Generate function call with pixel coord and payload reference
            std::stringstream resolveShaderCall;
            resolveShaderCall << "resolve_shader_main(pixelCoord, rayPayloadBuffer.payloads[rayIndex]);\n";

            const auto callPosition = resolveShaderTemplate.find("//___RESOLVE_SHADER_CALL___");
            if (callPosition != std::string::npos)
            {
                resolveShaderTemplate.replace(callPosition, std::string("//___RESOLVE_SHADER_CALL___").length(), resolveShaderCall.str());
            }
        }
        else
        {
            // No user resolve shader - use no-op passthrough
            // Remove placeholder comments
            size_t pos;
            while ((pos = resolveShaderTemplate.find("//___USER_PARAMS___")) != std::string::npos)
            {
                resolveShaderTemplate.replace(pos, std::string("//___USER_PARAMS___").length(), "// No user parameters - using no-op passthrough");
            }
            while ((pos = resolveShaderTemplate.find("//___RESOLVE_SHADER_SIGNATURE___")) != std::string::npos)
            {
                resolveShaderTemplate.replace(pos, std::string("//___RESOLVE_SHADER_SIGNATURE___").length(), "");
            }
            while ((pos = resolveShaderTemplate.find("//___RESOLVE_SHADER_FUNCTION___")) != std::string::npos)
            {
                resolveShaderTemplate.replace(pos, std::string("//___RESOLVE_SHADER_FUNCTION___").length(), "");
            }
            while ((pos = resolveShaderTemplate.find("//___RESOLVE_SHADER_CALL___")) != std::string::npos)
            {
                resolveShaderTemplate.replace(pos, std::string("//___RESOLVE_SHADER_CALL___").length(), "");
            }
        }

        printf("Vulkan Compute Ray Tracing Resolve Shader Source:\n%s\n", resolveShaderTemplate.c_str());

        // Compile finalShader using shaderc
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);

        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(resolveShaderTemplate.c_str(), resolveShaderTemplate.size(), shaderc_compute_shader, "ResolveShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Resolve Shader compilation failed: " + module.GetErrorMessage());
        }

        std::vector<uint32_t> spirvCode(module.cbegin(), module.cend());
        return spirvCode;
    }
}
