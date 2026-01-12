#include "../src/device/device.hpp"
#include "../src/device/buffer.hpp"
#include "../src/device/image_2d.hpp"
#include "../src/ray_tracing/ray_tracing_command_buffer/ray_tracing_command_buffer.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline.hpp"
#include "../src/ray_tracing/shader_module/shader_module.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/descriptor_set.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/shader_binding_table.hpp"
#include "../src/device/bottom_level_acceleration_structure.hpp"
#include "../src/device/top_level_acceleration_structure.hpp"

#include <fstream>
#include <iostream>

int main()
{
    std::unique_ptr<tracey::Device> computeDevice = std::unique_ptr<tracey::Device>(tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute));

    const std::unique_ptr<tracey::Buffer> vertexBuffer = std::unique_ptr<tracey::Buffer>(computeDevice->createBuffer(36 * sizeof(tracey::Vec3), tracey::BufferUsage::AccelerationStructureBuildInput | tracey::BufferUsage::StorageBuffer));
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
        vertexBuffer->flush();
    }

    std::unique_ptr<tracey::BottomLevelAccelerationStructure> blas = std::unique_ptr<tracey::BottomLevelAccelerationStructure>(computeDevice->createBottomLevelAccelerationStructure(vertexBuffer.get(), 36, sizeof(tracey::Vec3), nullptr, 0));
    std::array<tracey::Tlas::Instance, 2> instances;
    instances[0].blasAddress = 0;
    instances[0].instanceCustomIndexAndMask = 0;
    instances[0].setTransform(glm::translate(tracey::Vec3(0, 0, 5)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(0, 1, 0)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(-1, 0, 0)));
    instances[1].blasAddress = 0;
    instances[1].instanceCustomIndexAndMask = 1;
    instances[1].setTransform(glm::translate(tracey::Vec3(0, -3, 5)) * glm::scale(tracey::Vec3(10, 1, 10)));

    std::array<const tracey::BottomLevelAccelerationStructure *, 1> blasPtr = {blas.get()};

    std::unique_ptr<tracey::TopLevelAccelerationStructure> tlas = std::unique_ptr<tracey::TopLevelAccelerationStructure>(computeDevice->createTopLevelAccelerationStructure(std::span<const tracey::BottomLevelAccelerationStructure *>(blasPtr), instances));
    std::unique_ptr<tracey::Image2D> outputImage = std::unique_ptr<tracey::Image2D>(computeDevice->createImage2D(512, 512, tracey::ImageFormat::R8G8B8A8Unorm));

    tracey::RayTracingPipelineLayoutDescriptor layout;
    layout.addImage2D("outputImage", tracey::ShaderStage::RayGeneration);
    tracey::StructureLayout bufferStructure("Vertex");
    bufferStructure.addMember({"positions", "vec3", 0, true, 0});
    layout.addStorageBuffer("vertexBuffer", tracey::ShaderStage::ClosestHit, bufferStructure);
    layout.addAccelerationStructure("tlas", tracey::ShaderStage::RayGeneration);
    tracey::StructureLayout payloadLayout("RayPayload");
    payloadLayout.addMember({"color", "vec3", false, false});
    payloadLayout.addMember({"direction", "vec3", false, false});
    payloadLayout.addMember({"hit", "bool", false, false});
    layout.addPayload("rayPayload", tracey::ShaderStage::RayGeneration, payloadLayout);

    std::unique_ptr<tracey::ShaderModule> rayGenModule = std::unique_ptr<tracey::ShaderModule>(computeDevice->createShaderModule(tracey::ShaderStage::RayGeneration,
                                                                                                                                 R"(
Ray shader(uvec2 pixelCoord) {
    Ray ray;
    return ray;
}
    )",
                                                                                                                                 "shader"));

    std::unique_ptr<tracey::ShaderModule> closestHitModule = std::unique_ptr<tracey::ShaderModule>(computeDevice->createShaderModule(tracey::ShaderStage::ClosestHit,
                                                                                                                                     R"(
void shader() {
    // Shading goes here + generating a new ray
}
    )",
                                                                                                                                     "shader"));

    std::unique_ptr<tracey::ShaderModule> primaryMissModule = std::unique_ptr<tracey::ShaderModule>(computeDevice->createShaderModule(tracey::ShaderStage::Miss,
                                                                                                                                      R"(
void shader() {
    // Miss shader code
    // set up sky color based on ray direction
    float t = 0.5 * (rayPayload.direction.y + 1.0);
    vec3 sky = (1.0 - t) * vec3(1.0) + t * vec3(0.5, 0.7, 1.0);
    rayPayload.color = sky;
    rayPayload.hit = false;
}
    )",
                                                                                                                                      "shader"));

    std::array<const tracey::ShaderModule *, 1> hitModules = {closestHitModule.get()};
    std::array<const tracey::ShaderModule *, 1> missModules = {primaryMissModule.get()};
    std::unique_ptr<tracey::ShaderBindingTable> sbt = std::unique_ptr<tracey::ShaderBindingTable>(computeDevice->createShaderBindingTable(rayGenModule.get(), hitModules, missModules));
    // This is where the shaders are compiled
    std::unique_ptr<tracey::RayTracingPipeline> pipeline = std::unique_ptr<tracey::RayTracingPipeline>(computeDevice->createWaveFrontRayTracingPipeline(layout, sbt.get()));

    std::array<tracey::DescriptorSet *, 1> descriptorSets;
    pipeline->allocateDescriptorSets(descriptorSets);
    std::array<std::unique_ptr<tracey::DescriptorSet>, 1> descriptorSetOwners;
    for (size_t i = 0; i < descriptorSets.size(); ++i)
    {
        descriptorSetOwners[i] = std::unique_ptr<tracey::DescriptorSet>(descriptorSets[i]);
    }
    descriptorSetOwners[0]->setImage2D("outputImage", outputImage.get());
    descriptorSetOwners[0]->setAccelerationStructure("tlas", tlas.get());
    descriptorSetOwners[0]->setBuffer("vertexBuffer", vertexBuffer.get());

    std::unique_ptr<tracey::RayTracingCommandBuffer> commandBuffer = std::unique_ptr<tracey::RayTracingCommandBuffer>(computeDevice->createRayTracingCommandBuffer());
    commandBuffer->begin();
    commandBuffer->setPipeline(pipeline.get());
    commandBuffer->setDescriptorSet(descriptorSetOwners[0].get());
    commandBuffer->traceRays(*sbt, 512, 512);

    // Read back the image data
    const auto imageBufferSize = 512 * 512 * 4;
    std::unique_ptr<tracey::Buffer> imageReadbackBuffer = std::unique_ptr<tracey::Buffer>(computeDevice->createBuffer(imageBufferSize, tracey::BufferUsage::TransferDst));
    commandBuffer->copyImageToBuffer(outputImage.get(), imageReadbackBuffer.get());
    commandBuffer->end();
    std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();
    commandBuffer->waitUntilCompleted();
    std::chrono::high_resolution_clock::time_point endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> executionTime = endTime - startTime;
    std::cout << "Ray tracing execution time: " << executionTime.count() << " ms" << std::endl;

    const auto imageData = static_cast<const char *>(imageReadbackBuffer->mapForReading());
    // Save imageData to a ppm file
    std::ofstream outFile("output.ppm", std::ios::binary);
    // write only rgb channels
    outFile << "P6\n"
            << 512 << " " << 512 << "\n255\n";
    for (uint32_t y = 0; y < 512; ++y)
    {
        for (uint32_t x = 0; x < 512; ++x)
        {
            uint8_t r = imageData[(y * 512 + x) * 4 + 0];
            uint8_t g = imageData[(y * 512 + x) * 4 + 1];
            uint8_t b = imageData[(y * 512 + x) * 4 + 2];
            outFile << r << g << b;
        }
    }
    outFile.close();
    return 0;
}