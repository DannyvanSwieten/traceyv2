#pragma once

#include "../scene/scene_compiler.hpp"
#include "../scene/camera.hpp"
#include "../device/device.hpp"
#include "../device/image_2d.hpp"
#include "../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline.hpp"
#include "../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../ray_tracing/ray_tracing_pipeline/descriptor_set.hpp"
#include "../ray_tracing/ray_tracing_command_buffer/ray_tracing_command_buffer.hpp"
#include "../ray_tracing/isf/isf_pipeline_builder.hpp"
#include "../ray_tracing/isf/shader_inputs_buffer.hpp"

#include <memory>
#include <filesystem>
#include <array>

namespace tracey
{
    /// Configuration for path tracer rendering
    struct PathTracerConfig
    {
        // Output resolution
        uint32_t width = 512;
        uint32_t height = 512;

        // Shader file paths
        std::filesystem::path rayGenShader;
        std::filesystem::path hitShader;
        std::filesystem::path missShader;
        std::filesystem::path resolveShader;

        // HDR output (R32G32B32A32Sfloat vs R8G8B8A8Unorm)
        bool hdrOutput = true;

        // Render quality settings
        uint32_t samplesPerFrame = 16;
        uint32_t maxBounces = 8;
    };

    /// High-level path tracing renderer that encapsulates the entire rendering pipeline
    class PathTracer
    {
    public:
        /// Construct a path tracer with rendering configuration
        /// Scene is NOT coupled at construction - passed to render() instead
        /// @param device The device to use for rendering (not owned)
        /// @param config Rendering configuration (shaders, resolution, etc.)
        PathTracer(Device *device, const PathTracerConfig &config);

        ~PathTracer();

        // Non-copyable, movable
        PathTracer(const PathTracer &) = delete;
        PathTracer &operator=(const PathTracer &) = delete;
        PathTracer(PathTracer &&) = default;
        PathTracer &operator=(PathTracer &&) = default;

        /// Render a scene with the given camera
        /// @param scene The compiled scene to render
        /// @param camera The camera to render from
        /// @param clearAccumulation If true, clears previous samples (default true)
        /// @return Rendering time in milliseconds
        double render(const SceneCompiler::CompiledScene &scene,
                      const Camera &camera,
                      bool clearAccumulation = true);

        /// Get the output image (HDR or LDR depending on config)
        /// Valid after calling render()
        Image2D *outputImage() const { return m_outputImage.get(); }

        /// Read back the output image to CPU memory
        /// @param outData Pointer to receive the image data (caller must allocate width*height*4*sizeof(float or uint8_t))
        /// @return Size of data copied in bytes
        size_t readback(void *outData);

        /// Get shader inputs buffer for advanced use cases
        /// Allows direct manipulation of shader uniforms beyond camera parameters
        ShaderInputsBuffer *shaderInputs() { return m_shaderInputs.get(); }

        /// Get current resolution
        uint32_t width() const { return m_config.width; }
        uint32_t height() const { return m_config.height; }

        /// Get total accumulated sample count (render iterations * samples per frame)
        uint32_t sampleCount() const { return m_sampleCount * m_config.samplesPerFrame; }

        /// Get/set samples per frame (used during rendering)
        uint32_t samplesPerFrame() const { return m_config.samplesPerFrame; }
        void setSamplesPerFrame(uint32_t samples) { m_config.samplesPerFrame = samples; }

        /// Get/set max bounces (ray depth)
        uint32_t maxBounces() const { return m_config.maxBounces; }
        void setMaxBounces(uint32_t bounces) { m_config.maxBounces = bounces; }

    private:
        // Setup methods called from constructor
        void createOutputImage();
        void buildPipeline();
        void allocateDescriptorSets();
        void bindSceneResources(const SceneCompiler::CompiledScene &scene);
        void updateCameraUniforms(const Camera &camera);
        void updateEnvironmentUniforms(const SceneCompiler::CompiledScene &scene);

        // Not owned
        Device *m_device;

        // Configuration
        PathTracerConfig m_config;

        // Pipeline resources (owned)
        std::unique_ptr<ISFPipelineBuilder> m_pipelineBuilder;
        std::unique_ptr<RayTracingPipelineLayoutDescriptor> m_pipelineLayout;
        std::unique_ptr<RayTracingPipeline> m_pipeline;
        std::unique_ptr<ShaderInputsBuffer> m_shaderInputs;

        // Descriptor sets (owned)
        std::array<std::unique_ptr<DescriptorSet>, 2> m_descriptorSets;

        // Command buffer and output (owned)
        std::unique_ptr<RayTracingCommandBuffer> m_commandBuffer;
        std::unique_ptr<Image2D> m_outputImage;
        std::unique_ptr<Buffer> m_readbackBuffer;

        // Rendering state
        uint32_t m_sampleCount = 0;
    };
} // namespace tracey
