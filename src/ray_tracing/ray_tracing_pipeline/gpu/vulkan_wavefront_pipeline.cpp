#include "vulkan_wavefront_pipeline.hpp"
#include "vulkan_compute_pipeline_compiler.hpp"

namespace tracey
{
    VulkanWaveFrontPipeline::VulkanWaveFrontPipeline(VulkanComputeDevice &device, const RayTracingPipelineLayoutDescriptor &layout, const CpuShaderBindingTable &sbt)
    {
        const auto rayGenSpirV = compileRayGenShader(layout, sbt);
    }
}