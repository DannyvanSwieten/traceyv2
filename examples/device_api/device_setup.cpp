#include "../src/device/device.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"

int main()
{
    tracey::Device *cpuComputeDevice = tracey::createDevice(tracey::DeviceType::Cpu, tracey::DeviceBackend::None);
    tracey::RayTracingPipelineLayout layout;
    layout.addImage2D("outputImage", 0, tracey::ShaderStage::RayGeneration);
    layout.addAccelerationStructure("tlas", 1, tracey::ShaderStage::RayGeneration);

    tracey::BufferLayout bufferStructure("Vertex");
    bufferStructure.addMember({"position", "vec3", false, 0});
    bufferStructure.addMember({"normal", "vec3", false, 0});
    layout.addBuffer("vertexBuffer", 2, tracey::ShaderStage::ClosestHit, bufferStructure);

    std::array<tracey::DescriptorSet *, 1> descriptorSets;
    cpuComputeDevice->allocateDescriptorSets(descriptorSets, layout);

    auto rayGenModule = cpuComputeDevice->createShaderModule(tracey::ShaderStage::RayGeneration,
                                                             R"(
void shader() {
    // Ray generation shader code
}
    )",
                                                             "shader");

    auto closestHitModule = cpuComputeDevice->createShaderModule(tracey::ShaderStage::ClosestHit,
                                                                 R"(
void shader() {
    // Closest hit shader code
    
}
    )",
                                                                 "shader");

    std::array<const tracey::ShaderModule *, 1> hitModules = {closestHitModule};
    auto sbt = cpuComputeDevice->createShaderBindingTable(rayGenModule, hitModules);
    // This is where the shaders are compiled
    auto pipeline = cpuComputeDevice->createRayTracingPipeline(layout, sbt);

    auto commandBuffer = cpuComputeDevice->createRayTracingCommandBuffer();

    delete cpuComputeDevice;
    return 0;
}