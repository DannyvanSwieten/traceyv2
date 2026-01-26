#pragma once

#include "../scene/scene_compiler.hpp"
#include "../scene/camera.hpp"
#include "../device/device.hpp"
#include "../device/image_2d.hpp"
#include "graphics_pipeline.hpp"
#include "graphics_pipeline_layout.hpp"
#include "graphics_command_buffer.hpp"

#include <memory>
#include <filesystem>

namespace tracey
{
    /// Configuration for rasterization renderer
    struct RasterizerConfig
    {
        // Output resolution
        uint32_t width = 1280;
        uint32_t height = 720;

        // Shader file paths
        std::filesystem::path vertexShader;
        std::filesystem::path fragmentShader;

        // Rendering options
        bool useDepthBuffer = true;
        bool depthTestEnable = true;
        bool cullBackFaces = true;
        bool alphaBlending = false;

        // Output format
        ImageFormat colorFormat = ImageFormat::R8G8B8A8Unorm;
    };

    /// High-level rasterization renderer for realtime preview
    /// Provides fast rendering using traditional graphics pipeline
    class Rasterizer
    {
    public:
        /// Construct a rasterizer with rendering configuration
        /// Scene is NOT coupled at construction - passed to render() instead
        /// @param device The device to use for rendering (not owned)
        /// @param config Rendering configuration (shaders, resolution, etc.)
        Rasterizer(Device *device, const RasterizerConfig &config);

        ~Rasterizer();

        // Non-copyable, movable
        Rasterizer(const Rasterizer &) = delete;
        Rasterizer &operator=(const Rasterizer &) = delete;
        Rasterizer(Rasterizer &&) = default;
        Rasterizer &operator=(Rasterizer &&) = default;

        /// Render a scene with the given camera
        /// @param scene The compiled scene to render
        /// @param camera The camera to render from
        /// @return Rendering time in milliseconds
        double render(const SceneCompiler::CompiledScene &scene,
                      const Camera &camera);

        /// Get the output image
        /// Valid after calling render()
        Image2D *outputImage() const { return m_outputImage.get(); }

        /// Read back the output image to CPU memory
        /// @param outData Pointer to receive the image data (caller must allocate width*height*4*sizeof(uint8_t))
        /// @return Size of data copied in bytes
        size_t readback(void *outData);

        /// Get current resolution
        uint32_t width() const { return m_config.width; }
        uint32_t height() const { return m_config.height; }

    private:
        // Setup methods called from constructor
        void createPipeline();

        // Render helpers
        void updateCameraUniforms(const Camera &camera);
        void renderScene(const SceneCompiler::CompiledScene &scene);

        // Not owned
        Device *m_device;

        // Configuration
        RasterizerConfig m_config;

        // Pipeline resources (owned)
        std::unique_ptr<GraphicsPipelineLayout> m_pipelineLayout;
        std::unique_ptr<GraphicsPipeline> m_pipeline;
        std::unique_ptr<GraphicsCommandBuffer> m_commandBuffer;

        // Output image (color target is owned by pipeline)
        std::unique_ptr<Image2D> m_outputImage;
        std::unique_ptr<Buffer> m_readbackBuffer;

        // Camera matrices (computed in updateCameraUniforms, used in renderScene)
        glm::mat4 m_viewMatrix;
        glm::mat4 m_projectionMatrix;
    };
} // namespace tracey
