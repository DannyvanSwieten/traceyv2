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

        // Optional secondary point-sprite pass overlaid in the same render
        // pass. When set, callers can toggle `setShowPoints(true)` at runtime
        // to enable the second draw.
        std::filesystem::path pointsVertexShader;
        std::filesystem::path pointsFragmentShader;

        // Optional tertiary wireframe (POLYGON_MODE_LINE) pass overlaid in
        // the same render pass. Toggled at runtime via setShowEdges.
        std::filesystem::path linesVertexShader;
        std::filesystem::path linesFragmentShader;

        // Optional reference ground-grid pass on the y=0 plane. Toggled at
        // runtime via setShowGround.
        std::filesystem::path groundVertexShader;
        std::filesystem::path groundFragmentShader;

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
        /// Valid after calling render(). The pipeline owns this image; the
        /// rasterizer just exposes a view.
        Image2D *outputImage() const;

        /// Read back the output image to CPU memory
        /// @param outData Pointer to receive the image data (caller must allocate width*height*4*sizeof(uint8_t))
        /// @return Size of data copied in bytes
        size_t readback(void *outData);

        /// Get current resolution
        uint32_t width() const { return m_config.width; }
        uint32_t height() const { return m_config.height; }

        /// Toggle the secondary points overlay (alpha-blended circle splats
        /// drawn after the triangle pass). No-op if the points pipeline
        /// wasn't configured at construction.
        void setShowPoints(bool v) { m_showPoints = v; }
        bool showPoints() const { return m_showPoints; }

        /// Toggle the wireframe overlay (triangle edges via POLYGON_MODE_LINE).
        /// No-op if the lines pipeline wasn't configured at construction.
        void setShowEdges(bool v) { m_showEdges = v; }
        bool showEdges() const { return m_showEdges; }

        /// Toggle the reference ground-grid overlay (y=0 plane). No-op if the
        /// ground pipeline wasn't configured at construction.
        void setShowGround(bool v) { m_showGround = v; }
        bool showGround() const { return m_showGround; }

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

        // Color target is owned by m_pipeline; readback buffer is ours.
        std::unique_ptr<Buffer> m_readbackBuffer;

        bool m_showPoints = false;
        bool m_showEdges = false;
        bool m_showGround = false;

        // Camera matrices (computed in updateCameraUniforms, used in renderScene)
        glm::mat4 m_viewMatrix;
        glm::mat4 m_projectionMatrix;
    };
} // namespace tracey
