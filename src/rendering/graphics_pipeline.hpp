#pragma once
#include <filesystem>
#include <memory>
#include "graphics_pipeline_layout.hpp"
#include "../device/image_2d.hpp"

namespace tracey
{
    enum class PrimitiveTopology
    {
        TriangleList,
        PointList,
    };

    /// Configuration for creating a graphics pipeline
    struct GraphicsPipelineConfig
    {
        // Shader paths
        std::filesystem::path vertexShader;
        std::filesystem::path fragmentShader;

        // Render target configuration
        uint32_t width = 1280;
        uint32_t height = 720;
        ImageFormat colorFormat = ImageFormat::R8G8B8A8Unorm;
        bool useDepthBuffer = true;

        // Pipeline state
        bool depthTestEnable = true;
        bool depthWriteEnable = true;
        bool cullBackFaces = true;
        bool alphaBlending = false;
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;

        // Optional second pipeline drawing point sprites overlaid in the same
        // render pass. When set, the implementation creates a sibling pipeline
        // sharing the render pass + framebuffer + pipeline layout, with
        // POINT_LIST topology, alpha blending, and depth-test enabled but
        // depth-write disabled (so points don't punch through later draws).
        std::filesystem::path pointsVertexShader;
        std::filesystem::path pointsFragmentShader;

        // Optional third sibling pipeline drawing triangle edges (wireframe)
        // via POLYGON_MODE_LINE. Same vertex buffers as the triangle pipeline;
        // depth-test on, depth-write off, slight depth bias to prevent
        // z-fighting against the underlying filled triangles.
        std::filesystem::path linesVertexShader;
        std::filesystem::path linesFragmentShader;

        // Optional fourth sibling pipeline drawing a reference ground grid on
        // the y=0 plane. The vertex shader procedurally emits a 4-vertex quad
        // (TRIANGLE_STRIP, no vertex buffer); fragment shader draws an
        // anti-aliased grid that fades with distance. Depth-test on,
        // depth-write off, alpha blending so geometry intersecting Y=0 still
        // composites correctly.
        std::filesystem::path groundVertexShader;
        std::filesystem::path groundFragmentShader;

        // Vertex input layout (position only — vec3 per vertex, tight stride)
    };

    /// Abstract interface for graphics pipeline
    /// Encapsulates graphics pipeline state (shaders, render pass, descriptors)
    class GraphicsPipeline
    {
    public:
        virtual ~GraphicsPipeline() = default;

        /// Bind this pipeline for rendering
        /// Must be called before draw commands
        virtual void bind() = 0;

        /// Get the pipeline layout descriptor
        /// Used to create and bind descriptor sets
        virtual const GraphicsPipelineLayout& layout() const = 0;

        /// Get the output color image (framebuffer attachment)
        virtual Image2D* colorTarget() const = 0;

        /// Get the output depth image (if depth buffer enabled)
        virtual Image2D* depthTarget() const = 0;

        /// Get pipeline configuration
        virtual const GraphicsPipelineConfig& config() const = 0;
    };
}
