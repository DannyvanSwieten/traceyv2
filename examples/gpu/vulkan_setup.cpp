#include "../src/device/device.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"

int main()
{
    tracey::Device *cpuComputeDevice = tracey::createDevice(tracey::DeviceType::Cpu, tracey::DeviceBackend::None);
    tracey::RayTracingPipelineLayout layout;
    layout.addImage2D("outputImage", 0, tracey::ShaderStage::RayGeneration);
    layout.addAccelerationStructure("tlas", 2, tracey::ShaderStage::RayGeneration);
    layout.addBuffer("materials", 1, tracey::ShaderStage::ClosestHit);

    std::array<tracey::DescriptorSet *, 1> descriptorSets;
    cpuComputeDevice->allocateDescriptorSets(descriptorSets, layout);

    auto shaderModule = cpuComputeDevice->createShaderModule(layout, tracey::ShaderStage::RayGeneration,
                                                             R"(
void shader() {
    // Ray generation shader code
}
    )",
                                                             "shader");

    auto closestHitModule = cpuComputeDevice->createShaderModule(layout, tracey::ShaderStage::ClosestHit,
                                                                 R"(
void shader() {
    // Closest hit shader code
}
    )",
                                                                 "shader");

    std::array<const tracey::ShaderModule *, 1> hitShaders = {closestHitModule};
    auto sbt = cpuComputeDevice->createShaderBindingTable(shaderModule, hitShaders);
    delete cpuComputeDevice;
    return 0;
}