#include "../../src/gpu/vulkan_context.hpp"
#include "../../src/device/gpu/vulkan_compute_device.hpp"
#include "../../src/rendering/graphics_pipeline.hpp"
#include "../../src/rendering/graphics_pipeline_layout.hpp"
#include "../../src/rendering/graphics_command_buffer.hpp"
#include <iostream>
#include <filesystem>

using namespace tracey;

int main(int argc, char** argv)
{
    try
    {
        std::cout << "=== Graphics Pipeline Test ===" << std::endl;

        // Step 1: Create Vulkan context
        std::cout << "Creating Vulkan context..." << std::endl;
        VulkanContext context;
        std::cout << "  - Compute queue family: " << context.computeQueueFamilyIndex() << std::endl;
        std::cout << "  - Graphics queue family: " << context.graphicsQueueFamilyIndex() << std::endl;
        std::cout << "  - Unified queue: " << (context.hasUnifiedQueue() ? "Yes" : "No") << std::endl;

        // Step 2: Create device
        std::cout << "\nCreating device..." << std::endl;
        VulkanComputeDevice device(std::move(context));
        std::cout << "  - Device created successfully" << std::endl;

        // Step 3: Create graphics pipeline layout
        std::cout << "\nCreating pipeline layout..." << std::endl;
        GraphicsPipelineLayout layout;
        // For this test, we use an empty layout (no descriptors)
        // Push constants are defined in the shader but don't need descriptor layout
        std::cout << "  - Pipeline layout created" << std::endl;

        // Step 4: Create graphics pipeline config
        std::cout << "\nConfiguring graphics pipeline..." << std::endl;

        // Find shader directory
        std::filesystem::path shaderDir;
        if (argc > 1)
        {
            shaderDir = argv[1];
        }
        else
        {
            // Default to current directory (where the executable runs)
            shaderDir = std::filesystem::current_path();
        }

        GraphicsPipelineConfig config;
        config.width = 1280;
        config.height = 720;
        config.vertexShader = shaderDir / "simple.vert.spv";
        config.fragmentShader = shaderDir / "simple.frag.spv";
        config.colorFormat = ImageFormat::R8G8B8A8Unorm;
        config.useDepthBuffer = false;  // Disabled until image views are implemented
        config.depthTestEnable = false;
        config.cullBackFaces = true;

        std::cout << "  - Vertex shader: " << config.vertexShader << std::endl;
        std::cout << "  - Fragment shader: " << config.fragmentShader << std::endl;
        std::cout << "  - Resolution: " << config.width << "x" << config.height << std::endl;

        // Verify shaders exist
        if (!std::filesystem::exists(config.vertexShader))
        {
            std::cerr << "ERROR: Vertex shader not found: " << config.vertexShader << std::endl;
            std::cerr << "Current directory: " << std::filesystem::current_path() << std::endl;
            std::cerr << "Run compile_shaders.sh first!" << std::endl;
            return 1;
        }
        if (!std::filesystem::exists(config.fragmentShader))
        {
            std::cerr << "ERROR: Fragment shader not found: " << config.fragmentShader << std::endl;
            return 1;
        }

        // Step 5: Create graphics pipeline
        std::cout << "\nCreating graphics pipeline..." << std::endl;
        GraphicsPipeline* pipeline = device.createGraphicsPipeline(config, layout);
        std::cout << "  - Pipeline created successfully!" << std::endl;
        std::cout << "  - Color target: " << pipeline->colorTarget()->width() << "x"
                  << pipeline->colorTarget()->height() << std::endl;

        // Step 6: Create graphics command buffer
        std::cout << "\nCreating graphics command buffer..." << std::endl;
        GraphicsCommandBuffer* commandBuffer = device.createGraphicsCommandBuffer();
        std::cout << "  - Command buffer created successfully!" << std::endl;

        // Step 7: Test command recording (without actual rendering)
        std::cout << "\nTesting command recording..." << std::endl;
        commandBuffer->begin();
        std::cout << "  - Begin recording: OK" << std::endl;

        commandBuffer->beginRenderPass(pipeline, 0.2f, 0.3f, 0.4f, 1.0f, 1.0f);
        std::cout << "  - Begin render pass: OK" << std::endl;

        commandBuffer->bindPipeline(pipeline);
        std::cout << "  - Bind pipeline: OK" << std::endl;

        commandBuffer->endRenderPass();
        std::cout << "  - End render pass: OK" << std::endl;

        commandBuffer->end();
        std::cout << "  - End recording: OK" << std::endl;

        // Cleanup
        std::cout << "\nCleaning up..." << std::endl;
        delete commandBuffer;
        delete pipeline;
        std::cout << "  - Cleanup complete" << std::endl;

        std::cout << "\n=== SUCCESS: All tests passed! ===" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n=== ERROR: " << e.what() << " ===" << std::endl;
        return 1;
    }
}
