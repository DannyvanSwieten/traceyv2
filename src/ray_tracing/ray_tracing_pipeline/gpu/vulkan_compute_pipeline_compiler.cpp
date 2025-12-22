#include "vulkan_compute_pipeline_compiler.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include "../cpu/cpu_shader_binding_table.hpp"
#include "../../../ray_tracing/shader_module/cpu/cpu_shader_module.hpp"
#include <sstream>
#include <shaderc/shaderc.hpp>
namespace tracey
{
    void compileVulkanComputeRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt)
    {
        const auto rayGenShader = sbt.rayGen();

        std::stringstream ss;
        ss << "#version 460\n";
        ss << "layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;\n";

        ss << "struct BvhNode {\n";
        ss << "    vec3 aabbMin;\n";
        ss << "    uint leftChild;\n";
        ss << "    vec3 aabbMax;\n";
        ss << "    uint rightChild;\n";
        ss << "};\n";

        ss << "struct Ray {\n";
        ss << "    vec3 origin;\n";
        ss << "    vec3 direction;\n";
        ss << "    vec3 invDirection;\n";
        ss << "};\n";

        ss << "struct InstanceData {\n";
        ss << "    vec4 t0;\n";
        ss << "    vec4 t1;\n";
        ss << "    vec4 t2;\n";
        ss << "    uint customIndexAndMask;\n";
        ss << "    uint sbtOffsetAndFlags;\n";
        ss << "    uint blasAddress;\n";
        ss << "    uint padding;\n";
        ss << "};\n";

        ss << "#define gl_LaunchIDEXT gl_GlobalInvocationID" << "\n";
        ss << "#define gl_LaunchSizeEXT gl_NumWorkGroups * gl_WorkGroupSize" << "\n";

        for (const auto &binding : layout.bindings())
        {
            switch (binding.type)
            {
            case RayTracingPipelineLayout::DescriptorType::Image2D:
                ss << "layout(binding = " << binding.index << ", set = 0) uniform writeonly image2D " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayout::DescriptorType::Buffer:
                ss << "layout(binding = " << binding.index << ", set = 0) buffer " << binding.name << "Buffer {\n";
                for (const auto &field : binding.structure->fields())
                {
                    ss << "    " << field.type << " " << field.name << ";\n";
                }
                ss << "} " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayout::DescriptorType::AccelerationStructure:
            {
                const auto actualBinding = binding.index * 2; // Using two bindings for TLAS (instances and nodes)
                ss << "layout(binding = " << actualBinding << ", set = 1) buffer " << binding.name << "Instances {\n";
                ss << "    InstanceData instances[];\n";
                ss << "} " << binding.name << "Instances;\n";
                ss << "layout(binding = " << actualBinding + 1 << ", set = 1) buffer " << binding.name << "BvhNodes {\n";
                ss << "    BvhNode bvhNodes[];\n";
                ss << "} " << binding.name << "BvhNodes;\n";

                break;
            }
            default:
                throw std::runtime_error("Unsupported descriptor type in Vulkan Compute pipeline compiler");
            }
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

        const auto cpuModule = dynamic_cast<const CpuShaderModule *>(rayGenShader);

        std::string userSource(cpuModule->source());
        // replace user entry point with rayGenMain
        size_t entryPointPos = userSource.find(cpuModule->entryPoint());
        if (entryPointPos != std::string_view::npos)
        {
            userSource.replace(entryPointPos, cpuModule->entryPoint().size(), "rayGenMain");
        }
        ss << userSource;

        printf("Vulkan Compute Ray Tracing Shader Source:\n%s\n", ss.str().c_str());

        const std::string finalSource = ss.str();
        // Compile finalSource using shaderc
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        shaderc::SpvCompilationResult module =
            compiler.CompileGlslToSpv(finalSource.c_str(), finalSource.size(), shaderc_compute_shader, "RayGenShader", options);
        if (module.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error("Vulkan Compute Ray Tracing Pipeline compilation failed: " + module.GetErrorMessage());
        }
    }
}
