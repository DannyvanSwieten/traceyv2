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
    tracey::Device *computeDevice = tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute);

    const auto vertexBuffer = computeDevice->createBuffer(36 * sizeof(tracey::Vec3), tracey::BufferUsage::AccelerationStructureBuildInput | tracey::BufferUsage::StorageBuffer);
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

    auto blas = computeDevice->createBottomLevelAccelerationStructure(vertexBuffer, 36, sizeof(tracey::Vec3), nullptr, 0);
    std::array<tracey::Tlas::Instance, 2> instances;
    instances[0].blasAddress = 0;
    instances[0].instanceCustomIndexAndMask = 0;
    instances[0].setTransform(glm::translate(tracey::Vec3(0, 0, 5)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(0, 1, 0)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(-1, 0, 0)));
    instances[1].blasAddress = 0;
    instances[1].instanceCustomIndexAndMask = 1;
    instances[1].setTransform(glm::translate(tracey::Vec3(0, -3, 5)) * glm::scale(tracey::Vec3(10, 1, 10)));

    std::array<const tracey::BottomLevelAccelerationStructure *, 1> blasPtr = {blas};

    tracey::TopLevelAccelerationStructure *tlas = computeDevice->createTopLevelAccelerationStructure(std::span<const tracey::BottomLevelAccelerationStructure *>(blasPtr), instances);
    tracey::Image2D *outputImage = computeDevice->createImage2D(512, 512, tracey::ImageFormat::R8G8B8A8Unorm);

    tracey::RayTracingPipelineLayout layout;
    layout.addImage2D("outputImage", 0, tracey::ShaderStage::RayGeneration);
    layout.addAccelerationStructure("tlas", 1, tracey::ShaderStage::RayGeneration);

    tracey::StructureLayout bufferStructure("Vertex");
    bufferStructure.addMember({"position", "vec3", false, 0});
    layout.addBuffer("vertexBuffer", 2, tracey::ShaderStage::ClosestHit, bufferStructure);

    tracey::StructureLayout payloadLayout("RayPayload");
    payloadLayout.addMember({"color", "vec3", false, false});
    payloadLayout.addMember({"direction", "vec3", false, false});
    payloadLayout.addMember({"hit", "bool", false, false});
    layout.addPayload("rayPayload", 0, tracey::ShaderStage::RayGeneration, payloadLayout);

    std::array<tracey::DescriptorSet *, 1> descriptorSets;
    computeDevice->allocateDescriptorSets(descriptorSets, layout);
    descriptorSets[0]->setImage2D(0, outputImage);
    descriptorSets[0]->setAccelerationStructure(1, tlas);
    descriptorSets[0]->setBuffer(2, vertexBuffer);

    auto rayGenModule = computeDevice->createShaderModule(tracey::ShaderStage::RayGeneration,
                                                          R"(

float radians(float degrees) {
    return degrees * 3.14159265358979323846 / 180.0;
}
void shader() {
    // Ray generation shader code

    // Generate a pinhole camera ray
    uvec2 launchID = gl_LaunchIDEXT.xy;
    vec2 pixelCoord = vec2(launchID.x, launchID.y);
    uvec2 launchSize = gl_LaunchSizeEXT.xy;
    float width = float(launchSize.x);
    float height = float(launchSize.y);
    float fov = 45.0f;
    float aspectRatio = width / height;
    float px = (2.0f * ((pixelCoord.x + 0.5f) / width) - 1.0f) * tan(radians(fov) / 2.0f) * aspectRatio; 
    float py = (1.0f - 2.0f * ((pixelCoord.y + 0.5f) / height)) * tan(radians(fov) / 2.0f);

    vec3 origin = vec3(0.0f, 0.0f, 0.0f);
    vec3 direction = normalize(vec3(px, py, 1.0f));

    rayPayload.color = vec3(0.0f);
    rayPayload.direction = direction;
    rayPayload.hit = false;

    vec3 color = vec3(1.0f);

    traceRaysEXT(tlas, 0, 0, 0, 0, 0,
                  origin, 0.01f,
                  direction, 100.0f,
                  0);

    color = rayPayload.color;

    if(rayPayload.hit) {
        // send shadow ray
        vec3 lightDir = normalize(vec3(0.0f, 1.0f, -1.0f));
        origin = (origin + direction * gl_HitTEXT) + lightDir * 0.01f;
        direction = lightDir;
        rayPayload.hit = false;
        traceRaysEXT(tlas, 0, 0, 0, 0, 0,
                      origin, 0.01f,
                      lightDir, 100.0f,
                      0);
        
        if(rayPayload.hit) {
            color *= 0.2f; // in shadow
        } 
    }

    imageStore(outputImage, launchID.xy, vec4(color, 1.0));
}
    )",
                                                          "shader");

    auto closestHitModule = computeDevice->createShaderModule(tracey::ShaderStage::ClosestHit,
                                                              R"(
void shader() {
    // Closest hit shader code
    // fetch the positions
    int vertexIndex = gl_PrimitiveID * 3;
    vec3 v0 = gl_ObjectToWorldEXT * vertexBuffer[vertexIndex + 0].position;
    vec3 v1 = gl_ObjectToWorldEXT * vertexBuffer[vertexIndex + 1].position;
    vec3 v2 = gl_ObjectToWorldEXT * vertexBuffer[vertexIndex + 2].position;

    // Simple shading based on normal
    vec3 normal = normalize(cross(v1 - v0, v2 - v0));

    // Some fake lighting
    vec3 L = normalize(vec3(0.0f, 1.0f, -1.0f));
    float I = max(dot(L, normal), 0.0f);
    rayPayload.color = vec3(I * 0.5);
    rayPayload.hit = true;
}
    )",
                                                              "shader");

    auto primaryMissModule = computeDevice->createShaderModule(tracey::ShaderStage::Miss,
                                                               R"(
void shader() {
    // Miss shader code
    // set up sky color based on ray direction
    float t = 0.5f * (rayPayload.direction.y + 1.0f);
    vec3 sky = (1.0f - t) * vec3(1.0f) + t * vec3(0.5f, 0.7f, 1.0f);
    rayPayload.color = sky;
    rayPayload.hit = false;
}
    )",
                                                               "shader");

    std::array<const tracey::ShaderModule *, 1> hitModules = {closestHitModule};
    std::array<const tracey::ShaderModule *, 1> missModules = {primaryMissModule};
    auto sbt = computeDevice->createShaderBindingTable(rayGenModule, hitModules, missModules);
    // This is where the shaders are compiled
    auto pipeline = computeDevice->createRayTracingPipeline(layout, sbt);

    auto commandBuffer = computeDevice->createRayTracingCommandBuffer();
    commandBuffer->begin();
    commandBuffer->setPipeline(pipeline);
    commandBuffer->setDescriptorSet(descriptorSets[0]);
    commandBuffer->traceRays(*sbt, 512, 512);
    commandBuffer->end();

    const auto imageData = outputImage->data();
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

    delete computeDevice;
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