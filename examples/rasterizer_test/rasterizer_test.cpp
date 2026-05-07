#include "../../src/device/device.hpp"
#include "../../src/scene/scene.hpp"
#include "../../src/scene/scene_compiler.hpp"
#include "../../src/scene/camera.hpp"
#include "../../src/rendering/rasterizer.hpp"
#include "../../src/gpu/vulkan_context.hpp"
#include "../../src/device/gpu/vulkan_compute_device.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <memory>

using namespace tracey;

// Helper to save PPM image
void savePPM(const std::string& filename, const uint8_t* pixels, uint32_t width, uint32_t height)
{
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open " << filename << std::endl;
        return;
    }

    // PPM header
    file << "P6\n" << width << " " << height << "\n255\n";

    // Write pixels (flip vertically for correct orientation)
    for (int y = height - 1; y >= 0; --y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t idx = (y * width + x) * 4; // R8G8B8A8
            file.put(pixels[idx + 0]); // R
            file.put(pixels[idx + 1]); // G
            file.put(pixels[idx + 2]); // B
        }
    }

    std::cout << "Saved image to " << filename << std::endl;
}

int main(int argc, char** argv)
{
    try {
        std::cout << "=== Rasterizer End-to-End Test ===" << std::endl;

        // Step 1: Create device
        std::cout << "\n1. Creating Vulkan device..." << std::endl;
        VulkanContext context;
        auto device = std::make_unique<VulkanComputeDevice>(std::move(context));
        std::cout << "   ✓ Device created" << std::endl;

        // Step 2: Create scene with a simple cube
        std::cout << "\n2. Creating scene..." << std::endl;
        Scene scene;

        // Add a cube object
        scene.addObject("CubeMesh", SceneObject::createCube(1.0f));

        // Create actor and add instance
        auto* cubeActor = scene.createActor();
        SceneInstance instance("CubeMesh"); // Reference to the cube object
        cubeActor->addInstance(instance);

        // Set cube transform (move it back a bit so camera can see it)
        Transform cubeTransform;
        cubeTransform.setPosition(Vec3(0.0f, 0.0f, -3.0f));
        cubeActor->setTransform(cubeTransform);

        std::cout << "   ✓ Scene created with cube" << std::endl;

        // Step 3: Compile scene
        std::cout << "\n3. Compiling scene..." << std::endl;
        auto compiledScene = SceneCompiler::compile(device.get(), scene);
        std::cout << "   ✓ Scene compiled" << std::endl;
        std::cout << "   - Total triangles: " << compiledScene.totalTriangles << std::endl;
        std::cout << "   - BLASes: " << compiledScene.blases.size() << std::endl;

        // Step 4: Configure rasterizer
        std::cout << "\n4. Configuring rasterizer..." << std::endl;

        // Use simple test shaders (MoltenVK-compatible, no bindless textures)
        // TODO: PBR shaders need descriptor indexing which has MoltenVK limitations
        std::filesystem::path buildDir = std::filesystem::current_path();
        std::filesystem::path shaderPath = buildDir.parent_path() / "graphics_test";

        RasterizerConfig config;
        config.width = 800;
        config.height = 600;
        config.vertexShader = shaderPath / "simple.vert.spv";
        config.fragmentShader = shaderPath / "simple.frag.spv";
        config.useDepthBuffer = false; // Depth buffer not yet fully implemented
        config.depthTestEnable = false;
        config.cullBackFaces = true;

        std::cout << "   - Resolution: " << config.width << "x" << config.height << std::endl;
        std::cout << "   - Vertex shader: " << config.vertexShader << std::endl;
        std::cout << "   - Fragment shader: " << config.fragmentShader << std::endl;

        // Step 5: Create rasterizer
        std::cout << "\n5. Creating rasterizer..." << std::endl;
        auto rasterizer = std::make_unique<Rasterizer>(device.get(), config);
        std::cout << "   ✓ Rasterizer created" << std::endl;

        // Step 6: Set up camera
        std::cout << "\n6. Setting up camera..." << std::endl;
        Camera camera;
        camera.setPosition(Vec3(0.0f, 0.0f, 0.0f));
        camera.setRotation(Quaternion(1.0f, 0.0f, 0.0f, 0.0f)); // Identity (w, x, y, z)
        camera.setFov(45.0f);
        camera.setAspectRatio(static_cast<float>(config.width) / config.height);
        camera.setNearPlane(0.1f);
        camera.setFarPlane(100.0f);
        std::cout << "   ✓ Camera configured" << std::endl;

        // Step 7: Render frame
        std::cout << "\n7. Rendering frame..." << std::endl;
        double renderTime = rasterizer->render(compiledScene, camera);
        std::cout << "   ✓ Rendered in " << renderTime << " ms" << std::endl;

        // Step 8: Read back pixels
        std::cout << "\n8. Reading back pixels..." << std::endl;
        const size_t bufferSize = config.width * config.height * 4; // R8G8B8A8
        std::vector<uint8_t> pixels(bufferSize);
        size_t bytesRead = rasterizer->readback(pixels.data());
        std::cout << "   ✓ Read back " << bytesRead << " bytes" << std::endl;

        // Step 9: Save to file
        std::cout << "\n9. Saving output..." << std::endl;
        savePPM("rasterizer_output.ppm", pixels.data(), config.width, config.height);

        // Step 10: Validate output
        std::cout << "\n10. Validating output..." << std::endl;

        // Check if all pixels are not black/zero
        bool hasNonZeroPixels = false;
        for (size_t i = 0; i < bufferSize; i += 4) {
            if (pixels[i] != 0 || pixels[i+1] != 0 || pixels[i+2] != 0) {
                hasNonZeroPixels = true;
                break;
            }
        }

        if (hasNonZeroPixels) {
            std::cout << "   ✓ Output contains rendered pixels" << std::endl;
        } else {
            std::cout << "   ⚠ WARNING: Output appears to be all black" << std::endl;
            std::cout << "   This may indicate rendering pipeline issues" << std::endl;
        }

        std::cout << "\n=== SUCCESS: Rasterizer test completed ===" << std::endl;
        std::cout << "Output saved to: rasterizer_output.ppm" << std::endl;
        std::cout << "\nYou can view the image with:" << std::endl;
        std::cout << "  convert rasterizer_output.ppm rasterizer_output.png" << std::endl;
        std::cout << "  open rasterizer_output.png" << std::endl;

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "\n=== ERROR: " << e.what() << " ===" << std::endl;
        return 1;
    }
}
