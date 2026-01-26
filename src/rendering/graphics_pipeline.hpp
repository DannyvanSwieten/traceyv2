#pragma once
#include <filesystem>
#include <memory>
#include "graphics_pipeline_layout.hpp"
#include "../device/image_2d.hpp"

namespace tracey
{
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

        // Vertex input layout (fixed for now: position, normal, uv)
        // Future: make this configurable
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
