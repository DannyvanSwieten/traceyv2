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
    payloadLayout.addMember({"depth", "uint", false, false});
    payloadLayout.addMember({"alive", "bool", false, false}); // Renamed from "active" (reserved word)
    payloadLayout.addMember({"sampleIndex", "uint", false, false});
    payloadLayout.addMember({"rngSeed", "uint", false, false}); // Persistent RNG seed
    layout.addPayload("rayPayload", tracey::ShaderStage::RayGeneration, payloadLayout);

    std::unique_ptr<tracey::ShaderModule> rayGenModule = std::unique_ptr<tracey::ShaderModule>(computeDevice->createShaderModule(tracey::ShaderStage::RayGeneration,
                                                                                                                                 R"(
// Simple hash function for random number generation
float hash(uint seed) {
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / 4294967296.0;
}

void shader(uvec2 pixelCoord, inout RayPayloads payloads) {
    uvec2 resolution = getResolution();

    // Initialize RNG seed based on pixel coordinates and sample index
    payloads.rayPayload.rngSeed = pixelCoord.x + pixelCoord.y * resolution.x +
                                   payloads.rayPayload.sampleIndex * resolution.x * resolution.y;

    // Generate random sub-pixel jitter for anti-aliasing
    float jitterX = hash(payloads.rayPayload.rngSeed);
    float jitterY = hash(payloads.rayPayload.rngSeed + 1u);

    // Create a simple pinhole camera ray direction with jittered sample position
    float width = float(resolution.x);
    float height = float(resolution.y);
    float fov = 45.0;
    float aspectRatio = width / height;
    float px = (2.0 * ((float(pixelCoord.x) + jitterX) / width) - 1.0) * tan(radians(fov) / 2.0) * aspectRatio;
    float py = (1.0 - 2.0 * ((float(pixelCoord.y) + jitterY) / height)) * tan(radians(fov) / 2.0);

    vec3 origin = vec3(0.0, 0.0, 0.0);
    vec3 direction = normalize(vec3(px, py, 1.0));

    // Initialize payload for path tracing
    payloads.rayPayload.direction = direction;
    payloads.rayPayload.color = vec3(1.0); // Start with white - will be multiplied by albedo at each bounce
    payloads.rayPayload.depth = 0u;
    payloads.rayPayload.alive = true;
    payloads.rayPayload.sampleIndex += 1u;

    traceRay(origin, 0.01, direction, 1000.0);
}
    )",
                                                                                                                                 "shader"));

    std::unique_ptr<tracey::ShaderModule> closestHitModule = std::unique_ptr<tracey::ShaderModule>(computeDevice->createShaderModule(tracey::ShaderStage::ClosestHit,
                                                                                                                                     R"(
// ============================================================================
// Simple Hemisphere Sampling
// ============================================================================

#define PI 3.14159265359

// Persistent random number generator (evolves the seed)
float nextRandom(inout uint seed) {
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / 4294967296.0;
}

// Sample cosine-weighted hemisphere
vec3 sampleCosineHemisphere(float r1, float r2) {
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt(r2);
    float sinTheta = sqrt(1.0 - r2);
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Build tangent frame
void buildTangentFrame(vec3 N, out vec3 T, out vec3 B) {
    vec3 up = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

vec3 tangentToWorld(vec3 v, vec3 N, vec3 T, vec3 B) {
    return v.x * T + v.y * B + v.z * N;
}

// ============================================================================
// Hit Shader
// ============================================================================

void shader(HitInfo hitInfo, inout RayPayloads payloads) {
    const uint MAX_DEPTH = 5u;

    // Only process if ray is still alive
    if (!payloads.rayPayload.alive) return;

    // Normal is already in world space
    vec3 N = normalize(vec3(hitInfo.normalX, hitInfo.normalY, hitInfo.normalZ));

    // Get hit position
    vec3 hitPos = getWorldHitPosition(g_CurrentRayIndex, hitInfo);

    // Material albedo
    vec3 albedo = vec3(0.5);

    // Update throughput: multiply by albedo at each bounce
    // For Lambert BRDF with cosine-weighted sampling: (albedo/π * cos) / (cos/π) = albedo
    payloads.rayPayload.color *= albedo;

    // Increment depth
    payloads.rayPayload.depth += 1u;

    // Build tangent frame
    vec3 T, B;
    buildTangentFrame(N, T, B);

    // Generate random numbers using persistent seed
    float r1 = nextRandom(payloads.rayPayload.rngSeed);
    float r2 = nextRandom(payloads.rayPayload.rngSeed);

    // Sample cosine-weighted hemisphere
    vec3 localDir = sampleCosineHemisphere(r1, r2);
    vec3 L = tangentToWorld(localDir, N, T, B);

    // Spawn secondary ray
    traceRay(hitPos + N * 0.001, 0.01, L, 1000.0);
}
    )",
                                                                                                                                     "shader"));

    std::unique_ptr<tracey::ShaderModule> primaryMissModule = std::unique_ptr<tracey::ShaderModule>(computeDevice->createShaderModule(tracey::ShaderStage::Miss,
                                                                                                                                      R"(
void shader(inout RayPayloads payloads) {
    // Only process if ray is still alive
    if (!payloads.rayPayload.alive) return;

    // Miss shader code - ray escaped to sky, terminate it
    float t = 0.5 * (payloads.rayPayload.direction.y + 1.0);
    vec3 sky = (1.0 - t) * vec3(1.0) + t * vec3(0.5, 0.7, 1.0);
    payloads.rayPayload.color *= sky;  // Multiply by sky color
    payloads.rayPayload.alive = false; // Terminate ray
}
    )",
                                                                                                                                      "shader"));

    std::array<const tracey::ShaderModule *, 1> hitModules = {closestHitModule.get()};
    std::array<const tracey::ShaderModule *, 1> missModules = {primaryMissModule.get()};

    // Create a resolve shader for centralized image writing
    // The resolve shader reads the final payload data and writes to the output image
    // This is the recommended approach for wavefront rendering as it:
    // - Provides a single place for all image writes
    // - Enables accumulation for multi-frame rendering
    // - Allows post-processing (tone mapping, denoising, etc.)
    std::unique_ptr<tracey::ShaderModule> resolveModule = std::unique_ptr<tracey::ShaderModule>(
        computeDevice->createShaderModule(tracey::ShaderStage::Resolve, R"(
void shader(uvec2 pixel, in RayPayloads payloads) {
    // Read the final color from the payload (set by hit/miss shaders)
    // accumulate color over multiple samples if needed
    uint sampleIndex = payloads.rayPayload.sampleIndex;
    vec3 color = payloads.rayPayload.color * 0.25; // Simple averaging for 4 samples

    // Write to output image
    vec4 currentOutput = imageLoad(outputImage, ivec2(pixel));
    color += currentOutput.rgb;
    imageStore(outputImage, ivec2(pixel), vec4(color, 1.0));
}
        )",
                                          "shader"));

    // Create shader binding table with resolve shader
    std::unique_ptr<tracey::ShaderBindingTable> sbt = std::unique_ptr<tracey::ShaderBindingTable>(
        computeDevice->createShaderBindingTable(rayGenModule.get(), hitModules, missModules, resolveModule.get()));
    // This is where the shaders are compiled
    std::unique_ptr<tracey::RayTracingPipeline> pipeline = std::unique_ptr<tracey::RayTracingPipeline>(computeDevice->createWaveFrontRayTracingPipeline(layout, sbt.get()));

    // Allocate 2 descriptor sets for multi-bounce ping-pong
    std::array<tracey::DescriptorSet *, 2> descriptorSets;
    pipeline->allocateDescriptorSets(descriptorSets);
    std::array<std::unique_ptr<tracey::DescriptorSet>, 2> descriptorSetOwners;
    for (size_t i = 0; i < descriptorSets.size(); ++i)
    {
        descriptorSetOwners[i] = std::unique_ptr<tracey::DescriptorSet>(descriptorSets[i]);
    }

    // Bind user-defined resources to both descriptor sets
    for (size_t i = 0; i < descriptorSetOwners.size(); ++i)
    {
        descriptorSetOwners[i]->setImage2D("outputImage", outputImage.get());
        descriptorSetOwners[i]->setAccelerationStructure("tlas", tlas.get());
        descriptorSetOwners[i]->setBuffer("vertexBuffer", vertexBuffer.get());
    }

    std::unique_ptr<tracey::RayTracingCommandBuffer> commandBuffer = std::unique_ptr<tracey::RayTracingCommandBuffer>(computeDevice->createRayTracingCommandBuffer());
    commandBuffer->begin();
    commandBuffer->setPipeline(pipeline.get());

    // Set both descriptor sets (they will be alternated during multi-bounce)
    commandBuffer->setDescriptorSet(descriptorSetOwners[0].get());
    commandBuffer->setDescriptorSet(descriptorSetOwners[1].get());

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
    std::ofstream outFile("wavefront_output.ppm", std::ios::binary);
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