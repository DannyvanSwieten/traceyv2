#include "../src/device/device.hpp"
#include "../src/device/buffer.hpp"
#include "../src/device/image_2d.hpp"
#include "../src/ray_tracing/ray_tracing_command_buffer/ray_tracing_command_buffer.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline.hpp"
#include "../src/ray_tracing/shader_module/shader_module.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/descriptor_set.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/shader_binding_table.hpp"

#include <fstream>

int main()
{
    tracey::Device *cpuComputeDevice = tracey::createDevice(tracey::DeviceType::Cpu, tracey::DeviceBackend::None);

    const auto vertexBuffer = cpuComputeDevice->createBuffer(36 * sizeof(tracey::Vec3), tracey::BufferUsage::AccelerationStructureBuildInput | tracey::BufferUsage::StorageBuffer);
    // Fill vertex buffer with cube vertices
    {
        auto *data = static_cast<tracey::Vec3 *>(vertexBuffer->mapForWriting());
        std::array<tracey::Vec3, 36> cube = {
            // Front face
            tracey::Vec3{-1, -1, 1},
            tracey::Vec3{1, -1, 1},
            tracey::Vec3{1, 1, 1},
            tracey::Vec3{-1, -1, 1},
            tracey::Vec3{1, 1, 1},
            tracey::Vec3{-1, 1, 1},
            // Back face
            tracey::Vec3{1, -1, -1},
            tracey::Vec3{-1, -1, -1},
            tracey::Vec3{-1, 1, -1},
            tracey::Vec3{1, -1, -1},
            tracey::Vec3{-1, 1, -1},
            tracey::Vec3{1, 1, -1},
            // Left face
            tracey::Vec3{-1, -1, -1},
            tracey::Vec3{-1, -1, 1},
            tracey::Vec3{-1, 1, 1},
            tracey::Vec3{-1, -1, -1},
            tracey::Vec3{-1, 1, 1},
            tracey::Vec3{-1, 1, -1},
            // Right face
            tracey::Vec3{1, -1, 1},
            tracey::Vec3{1, -1, -1},
            tracey::Vec3{1, 1, -1},
            tracey::Vec3{1, -1, 1},
            tracey::Vec3{1, 1, -1},
            tracey::Vec3{1, 1, 1},
            // Top face
            tracey::Vec3{-1, 1, 1},
            tracey::Vec3{1, 1, 1},
            tracey::Vec3{1, 1, -1},
            tracey::Vec3{-1, 1, 1},
            tracey::Vec3{1, 1, -1},
            tracey::Vec3{-1, 1, -1},
            // Bottom face
            tracey::Vec3{-1, -1, -1},
            tracey::Vec3{1, -1, -1},
            tracey::Vec3{1, -1, 1},
            tracey::Vec3{-1, -1, -1},
            tracey::Vec3{1, -1, 1},
            tracey::Vec3{-1, -1, 1},
        };
        std::copy(cube.begin(), cube.end(), data);
    }

    auto blas = cpuComputeDevice->createBottomLevelAccelerationStructure(vertexBuffer, 36, sizeof(tracey::Vec3), nullptr, 0);
    std::array<tracey::Tlas::Instance, 1> instances;
    instances[0].blasAddress = 0;
    instances[0].setTransform(glm::mat4(1.0f));

    std::array<const tracey::BottomLevelAccelerationStructure *, 1> blasPtr = {blas};

    tracey::TopLevelAccelerationStructure *tlas = cpuComputeDevice->createTopLevelAccelerationStructure(std::span<const tracey::BottomLevelAccelerationStructure *>(blasPtr), instances);
    tracey::Image2D *outputImage = cpuComputeDevice->createImage2D(800, 600, tracey::ImageFormat::R8G8B8A8Unorm);

    tracey::RayTracingPipelineLayout layout;
    layout.addImage2D("outputImage", 0, tracey::ShaderStage::RayGeneration);
    layout.addAccelerationStructure("tlas", 1, tracey::ShaderStage::RayGeneration);

    tracey::BufferLayout bufferStructure("Vertex");
    bufferStructure.addMember({"position", "vec3", false, 0});
    layout.addBuffer("vertexBuffer", 2, tracey::ShaderStage::ClosestHit, bufferStructure);

    std::array<tracey::DescriptorSet *, 1> descriptorSets;
    cpuComputeDevice->allocateDescriptorSets(descriptorSets, layout);
    descriptorSets[0]->setImage2D(0, outputImage);
    descriptorSets[0]->setAccelerationStructure(1, tlas);

    auto rayGenModule = cpuComputeDevice->createShaderModule(tracey::ShaderStage::RayGeneration,
                                                             R"(
void shader() {
    // Ray generation shader code

    // Generate a pinhole camera ray
    uvec2 launchID = gl_LaunchIDEXT.xy;
    uvec2 launchSize = gl_LaunchSizeEXT.xy;
    float u = (float(launchID.x) + 0.5f) / float(launchSize.x);
    float v = (float(launchID.y) + 0.5f) / float(launchSize.y);
    vec3 origin = vec3(0.0f, 0.0f, 0.0f);
    vec3 direction = normalize(vec3(u - 0.5f, v - 0.5f, -1.0f));

    imageStore(outputImage, launchID.xy, vec4(direction.x, direction.y, 0.0, 1.0));

    traceRaysEXT(tlas, 0, 0, 0, 0, 0,
                  origin, 0.01f,
                  direction, 100.0f,
                  0);
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
    commandBuffer->begin();
    commandBuffer->setPipeline(pipeline);
    commandBuffer->setDescriptorSet(descriptorSets[0]);
    commandBuffer->traceRays(*sbt, 800, 600);
    commandBuffer->end();

    const auto imageData = outputImage->data();
    // Save imageData to a ppm file
    std::ofstream outFile("output.ppm", std::ios::binary);
    // write only rgb channels
    outFile << "P6\n"
            << 800 << " " << 600 << "\n255\n";
    for (uint32_t y = 0; y < 600; ++y)
    {
        for (uint32_t x = 0; x < 800; ++x)
        {
            uint8_t r = imageData[(y * 800 + x) * 4 + 0];
            uint8_t g = imageData[(y * 800 + x) * 4 + 1];
            uint8_t b = imageData[(y * 800 + x) * 4 + 2];
            outFile << r << g << b;
        }
    }
    outFile.close();

    delete cpuComputeDevice;
    delete commandBuffer;
    delete pipeline;
    delete sbt;
    delete closestHitModule;
    delete rayGenModule;
    for (auto set : descriptorSets)
    {
        delete set;
    }
    delete tlas;
    delete blas;
    delete outputImage;
    delete vertexBuffer;
    return 0;
}