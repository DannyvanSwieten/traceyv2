#pragma once

#include "scene/scene_compiler.hpp"
#include "scene/camera.hpp"
#include "device/device.hpp"
#include "device/image_2d.hpp"
#include "device/buffer.hpp"
#include "rendering/data_structure.hpp"
#include "shader_inputs_buffer.hpp"
#include "shading/material_program/material_program.hpp"
#include "path_tracer_backend.hpp"

#include <memory>

namespace tracey
{
    /// Configuration for path tracer rendering
    struct PathTracerConfig
    {
        // Output resolution
        uint32_t width = 512;
        uint32_t height = 512;

        // HDR output (R32G32B32A32Sfloat vs R8G8B8A8Unorm)
        bool hdrOutput = true;

        // Render quality settings
        uint32_t samplesPerFrame = 16;
        uint32_t maxBounces = 8;

        // When true the integrator emits AOV layers (albedo/normal/depth/
        // position/emission/instance-id) alongside the beauty image. Off for
        // the interactive viewport (avoids extra buffers + bandwidth); the
        // export / denoise paths turn it on. See AovKind in path_tracer_backend.hpp.
        bool enableAovs = false;

        // When true the final beauty write skips tonemap + gamma and emits
        // LINEAR radiance — what an EXR film output / the denoiser wants. The
        // viewport leaves this false for a display-ready tonemapped image.
        bool linearOutput = false;

        // If true, the pipeline binds the four MaterialProgram SSBOs and the
        // hit shader is expected to be the uber-VM hit. Defaults to false so
        // legacy hit shaders keep working unchanged.
        bool useMaterialPrograms = false;

        // Which renderer implementation to use. Auto picks the best backend
        // available on this machine (see backend_registry.hpp).
        PathTracerBackendKind backend = PathTracerBackendKind::Auto;
    };

    /// High-level path tracing renderer. Owns format-agnostic state (sample
    /// counter, ShaderInputs uniform, output resources per the backend's
    /// output kind) and delegates per-frame dispatch to a PathTracerBackend
    /// (Metal RT on macOS, native CPU fallback, Vulkan RT to come).
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
        /// @param wantReadback If true the dispatch also enqueues a copy of
        ///        the output image into the readback buffer, so a subsequent
        ///        readback() observes this frame. The live viewport skips
        ///        this; only the export / explicit render_frame command
        ///        paths set it to true.
        /// @return Rendering time in milliseconds
        double render(const SceneCompiler::CompiledScene &scene,
                      const Camera &camera,
                      bool clearAccumulation = true,
                      bool wantReadback = true);

        /// Get the presentable output image (HDR or LDR depending on config).
        /// Valid after calling render(). Depending on the backend's output
        /// kind this is either the façade-owned image or one the backend
        /// provides (e.g. an IOSurface-shared texture).
        Image2D *outputImage() const;

        /// Read back the output image to CPU memory
        /// @param outData Pointer to receive the image data (caller must allocate width*height*4*sizeof(float or uint8_t))
        /// @return Size of data copied in bytes
        size_t readback(void *outData);

        /// True when the backend computed AOV layers for the latest render
        /// (requires config.enableAovs). See AovKind.
        bool aovsAvailable() const;

        /// Read back one AOV layer (RGBA32F, width*height*4 floats). Returns
        /// bytes written, or 0 if AOVs are unavailable.
        size_t readbackAOV(AovKind aov, void *outData);

        /// Get shader inputs buffer for advanced use cases
        /// Allows direct manipulation of shader uniforms beyond camera parameters
        ShaderInputsBuffer *shaderInputs() { return m_shaderInputs.get(); }

        /// Get current resolution
        uint32_t width() const { return m_config.width; }
        uint32_t height() const { return m_config.height; }

        /// True when the output/readback is RGBA32F (16 bytes/px) vs RGBA8 (4).
        /// The export path forces this on (linear AOV output) independently of
        /// the caller's own hdr setting, so readback sizing must consult it.
        bool hdrOutput() const { return m_config.hdrOutput; }

        /// Get total accumulated sample count (render iterations * samples per frame)
        uint32_t sampleCount() const { return m_sampleCount * m_config.samplesPerFrame; }

        /// Get/set samples per frame (used during rendering)
        uint32_t samplesPerFrame() const { return m_config.samplesPerFrame; }
        void setSamplesPerFrame(uint32_t samples) { m_config.samplesPerFrame = samples; }

        /// Get/set max bounces (ray depth)
        uint32_t maxBounces() const { return m_config.maxBounces; }
        void setMaxBounces(uint32_t bounces) { m_config.maxBounces = bounces; }

        /// Replace the material program buffers with the given packed programs.
        /// Only valid when config.useMaterialPrograms is true. Clears
        /// accumulation on next render.
        void setMaterialPrograms(const MaterialProgramBuffer &programs);

        /// Update one parameter slot and re-upload the parameters buffer. The
        /// programs themselves are unchanged; this is the animation path used
        /// by the graph editor for sliders / keyframes.
        void setMaterialParameter(uint32_t programId, uint32_t paramIdx, const Vec4 &value);

    private:
        void createOutputImage();
        void createShaderInputs();
        void updateCameraUniforms(const Camera &camera, uint32_t lightCount);
        // CpuPixels backends: push the backend's host-memory frame into the
        // façade-owned Vulkan image for the viewport compositor.
        void uploadCpuOutput();

        // Not owned
        Device *m_device;

        // Configuration
        PathTracerConfig m_config;

        // Format-agnostic resources owned by the façade. The backend gets
        // pointers to these via PathTracerBackend::InitParams.
        std::unique_ptr<Image2D> m_outputImage;
        std::unique_ptr<Image2D> m_accumulatorImage;
        std::unique_ptr<Buffer> m_readbackBuffer;
        std::unique_ptr<ShaderInputsBuffer> m_shaderInputs;
        StructureLayout m_shaderInputsLayout;

        // CPU mirror of the program buffer so setMaterialParameter can resolve
        // (programId, paramIdx) → absolute parameter slot before forwarding.
        MaterialProgramBuffer m_currentPrograms;

        // Backend that owns pipeline / descriptor / command-buffer state.
        std::unique_ptr<PathTracerBackend> m_backend;

        // Rendering state
        uint32_t m_sampleCount = 0;
    };
} // namespace tracey
