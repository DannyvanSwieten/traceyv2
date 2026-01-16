#include "../src/device/device.hpp"
#include "../src/device/buffer.hpp"
#include "../src/device/image_2d.hpp"
#include "../src/ray_tracing/ray_tracing_command_buffer/ray_tracing_command_buffer.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/data_structure.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/descriptor_set.hpp"
#include "../src/ray_tracing/ray_tracing_pipeline/shader_binding_table.hpp"
#include "../src/ray_tracing/isf/isf_pipeline_builder.hpp"
#include "../src/ray_tracing/isf/shader_inputs_buffer.hpp"
#include "../src/device/bottom_level_acceleration_structure.hpp"
#include "../src/device/top_level_acceleration_structure.hpp"

#include <fstream>
#include <iostream>
#include <filesystem>

int main()
{
    std::unique_ptr<tracey::Device> computeDevice = std::unique_ptr<tracey::Device>(
        tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute));

    // Create vertex buffer with cube geometry
    const std::unique_ptr<tracey::Buffer> vertexBuffer = std::unique_ptr<tracey::Buffer>(
        computeDevice->createBuffer(36 * sizeof(tracey::Vec3),
                                    tracey::BufferUsage::AccelerationStructureBuildInput | tracey::BufferUsage::StorageBuffer));

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

    // Create acceleration structures
    std::unique_ptr<tracey::BottomLevelAccelerationStructure> blas =
        std::unique_ptr<tracey::BottomLevelAccelerationStructure>(
            computeDevice->createBottomLevelAccelerationStructure(vertexBuffer.get(), 36, sizeof(tracey::Vec3), nullptr, 0));

    std::array<tracey::Tlas::Instance, 2> instances;
    instances[0].blasAddress = 0;
    instances[0].instanceCustomIndexAndMask = 0;
    instances[0].setTransform(glm::translate(tracey::Vec3(0, 0, 5)) *
                              glm::rotate(glm::radians(30.0f), tracey::Vec3(0, 1, 0)) *
                              glm::rotate(glm::radians(30.0f), tracey::Vec3(-1, 0, 0)));
    instances[1].blasAddress = 0;
    instances[1].instanceCustomIndexAndMask = 1;
    instances[1].setTransform(glm::translate(tracey::Vec3(0, -3, 5)) * glm::scale(tracey::Vec3(10, 1, 10)));

    std::array<const tracey::BottomLevelAccelerationStructure *, 1> blasPtr = {blas.get()};
    std::unique_ptr<tracey::TopLevelAccelerationStructure> tlas =
        std::unique_ptr<tracey::TopLevelAccelerationStructure>(
            computeDevice->createTopLevelAccelerationStructure(
                std::span<const tracey::BottomLevelAccelerationStructure *>(blasPtr), instances));

    // Create output image
    std::unique_ptr<tracey::Image2D> outputImage =
        std::unique_ptr<tracey::Image2D>(computeDevice->createImage2D(512, 512, tracey::ImageFormat::R8G8B8A8Unorm));

    // Build pipeline using ISF files
    std::cout << "Building pipeline from ISF shader files..." << std::endl;

    // Create the layout with user-provided resources (vertex buffer, TLAS, output image)
    // The ISF builder will add the payload extracted from ISF files
    tracey::RayTracingPipelineLayoutDescriptor layout;
    layout.addImage2D("outputImage", tracey::ShaderStage::RayGeneration);
    tracey::StructureLayout vertexStructure("Vertex");
    vertexStructure.addMember({"positions", "vec3", 0, true, 0});
    layout.addStorageBuffer("vertexBuffer", tracey::ShaderStage::ClosestHit, vertexStructure);
    layout.addAccelerationStructure("tlas", tracey::ShaderStage::RayGeneration);

    tracey::ISFPipelineBuilder builder(*computeDevice);

    // Get the path to the shader files (relative to executable)
    std::filesystem::path shaderDir = std::filesystem::path(__FILE__).parent_path() / "shaders";

    builder.addRayGenShader(shaderDir / "ray_gen.isf");
    builder.addHitShader(shaderDir / "diffuse_hit.isf");
    builder.addMissShader(shaderDir / "sky_miss.isf");
    builder.addResolveShader(shaderDir / "resolve.isf");

    // Create shader inputs buffer using the layout extracted from ISF files
    // This automatically handles std140 padding and lets us set values by name
    tracey::ShaderInputsBuffer shaderInputs(computeDevice.get(), builder.getInputsLayout());
    shaderInputs.setFloat("fov", 45.0f);                               // From ray_gen.isf
    shaderInputs.setVec4("albedo", glm::vec4(0.5f, 0.5f, 0.5f, 1.0f)); // RED for testing
    shaderInputs.upload();

    // Build pipeline - ISF builder adds payload from ISF files to our layout
    std::unique_ptr<tracey::RayTracingPipeline> pipeline = builder.build(layout);

    std::cout << "Pipeline built successfully!" << std::endl;

    // Allocate descriptor sets
    std::array<tracey::DescriptorSet *, 2> descriptorSets;
    pipeline->allocateDescriptorSets(descriptorSets);
    std::array<std::unique_ptr<tracey::DescriptorSet>, 2> descriptorSetOwners;
    for (size_t i = 0; i < descriptorSets.size(); ++i)
    {
        descriptorSetOwners[i] = std::unique_ptr<tracey::DescriptorSet>(descriptorSets[i]);
    }

    // Bind resources to descriptor sets
    for (size_t i = 0; i < descriptorSetOwners.size(); ++i)
    {
        descriptorSetOwners[i]->setImage2D("outputImage", outputImage.get());
        descriptorSetOwners[i]->setAccelerationStructure("tlas", tlas.get());
        descriptorSetOwners[i]->setBuffer("vertexBuffer", vertexBuffer.get());
        descriptorSetOwners[i]->setUniformBuffer("shaderInputs", shaderInputs.buffer());
    }

    // Create and execute command buffer
    std::unique_ptr<tracey::RayTracingCommandBuffer> commandBuffer =
        std::unique_ptr<tracey::RayTracingCommandBuffer>(computeDevice->createRayTracingCommandBuffer());

    commandBuffer->begin();
    commandBuffer->setPipeline(pipeline.get());
    commandBuffer->setDescriptorSet(descriptorSetOwners[0].get());
    commandBuffer->setDescriptorSet(descriptorSetOwners[1].get());
    commandBuffer->traceRays(*builder.getShaderBindingTable(), 512, 512);

    // Read back the image
    const auto imageBufferSize = 512 * 512 * 4;
    std::unique_ptr<tracey::Buffer> imageReadbackBuffer =
        std::unique_ptr<tracey::Buffer>(computeDevice->createBuffer(imageBufferSize, tracey::BufferUsage::TransferDst));
    commandBuffer->copyImageToBuffer(outputImage.get(), imageReadbackBuffer.get());
    commandBuffer->end();

    // Execute and time
    std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();
    commandBuffer->waitUntilCompleted();
    std::chrono::high_resolution_clock::time_point endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> executionTime = endTime - startTime;
    std::cout << "Ray tracing execution time: " << executionTime.count() << " ms" << std::endl;

    // Save output image
    const auto imageData = static_cast<const char *>(imageReadbackBuffer->mapForReading());
    std::ofstream outFile("wavefront_isf_output.ppm", std::ios::binary);
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

    std::cout << "Output saved to wavefront_isf_output.ppm" << std::endl;

    return 0;
}
