#include "vulkan_compute_pipeline_compiler.hpp"
#include "../ray_tracing_pipeline_layout.hpp"
#include "../cpu/cpu_shader_binding_table.hpp"
#include "../../../ray_tracing/shader_module/cpu/cpu_shader_module.hpp"
#include <sstream>
namespace tracey
{
    void compileCpuRayTracingPipeline(const RayTracingPipelineLayout &layout, const CpuShaderBindingTable &sbt)
    {
        const auto rayGenShader = sbt.rayGen();

        std::stringstream ss;
        ss << "#version 460\n";
        ss << "#extension GL_EXT_UINT_64 : require\n";
        ss << "layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;\n";

        ss << "struct BvhNode {\n";
        ss << "    vec3 aabbMin;\n";
        ss << "    uint leftChild;\n";
        ss << "    vec3 aabbMax;\n";
        ss << "    uint rightChild;\n";
        ss << "};\n";

        ss << "struct InstanceData {\n";
        ss << "    mat4x3 transform;\n";
        ss << "    uint instanceCustomIndexAndMask;\n";
        ss << "    uint instanceOffset;\n";
        ss << "    uint flags;\n";
        ss << "    uint64_t blasAddress;\n";
        ss << "};\n";

        for (const auto &binding : layout.bindings())
        {
            switch (binding.type)
            {
            case RayTracingPipelineLayout::DescriptorType::Image2D:
                ss << "layout(binding = " << binding.index << ", rgba8) uniform writeonly image2D " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayout::DescriptorType::Buffer:
                ss << "layout(binding = " << binding.index << ") buffer " << binding.name << "Buffer {\n";
                for (const auto &field : binding.structure->fields())
                {
                    ss << "    " << field.type << " " << field.name << ";\n";
                }
                ss << "} " << binding.name << ";\n";
                break;
            case RayTracingPipelineLayout::DescriptorType::AccelerationStructure:
                ss << "layout(binding = " << binding.index << ") buffer " << binding.name << "Buffer {\n";
                ss << "    InstanceData instances[];\n";
                ss << "} " << binding.name << ";\n";
                break;
            default:
                throw std::runtime_error("Unsupported descriptor type in Vulkan Compute pipeline compiler");
            }
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
    }
}
