#pragma once

#include "device/device.hpp"
#include "device/image_2d.hpp"
#include "device/buffer.hpp"
#include "scene/scene_compiler.hpp"
#include "shading/material_program/material_program.hpp"
#include "rendering/data_structure.hpp"
#include "shader_inputs_buffer.hpp"

#include <memory>

namespace tracey
{
    struct PathTracerConfig;

    // Which renderer implementation drives the path tracer. `Auto` resolves
    // to the best available backend for the platform (see backend_registry).
    enum class PathTracerBackendKind
    {
        Auto,
        MetalRT,  // macOS, Metal ray tracing (hardware on M3+)
        VulkanRT, // VK_KHR_ray_tracing (Windows/Linux RT GPUs) — stub
        Cpu,      // native CPU fallback
    };

    // Who owns the presentable output image, and in what form the backend
    // delivers pixels. Decides which InitParams resources the façade creates.
    enum class PathTracerOutputKind
    {
        // Backend writes into the façade-owned output/accumulator images and
        // copies into the façade-owned readback buffer (the future VulkanRT
        // backend renders this way).
        FacadeImage,
        // Backend owns a presentable Image2D (e.g. Metal RT rendering into an
        // IOSurface-backed texture surfaced as a VulkanImage2D). The façade
        // creates no images; backendOutputImage() must return non-null after
        // initialize().
        BackendImage,
        // Backend produces a CPU pixel buffer (CPU path tracer). The façade
        // owns only a Vulkan output image used as an upload target for the
        // viewport compositor; cpuOutputPixels() must be valid after each
        // dispatch().
        CpuPixels,
    };

    // Abstract backend that owns the renderer-side pipeline (compute wavefront,
    // hardware ray tracing, etc). The PathTracer façade owns format-agnostic
    // state (sample counter, ShaderInputs uniform, and — depending on the
    // backend's PathTracerOutputKind — the output/accumulator/readback
    // resources) and delegates per-frame dispatch to the backend.
    class PathTracerBackend
    {
    public:
        struct InitParams
        {
            Device *device = nullptr;                              // not owned
            const PathTracerConfig *config = nullptr;              // not owned
            // Null unless outputKind() requires them: FacadeImage gets all
            // three; CpuPixels gets outputImage only; BackendImage gets none.
            Image2D *outputImage = nullptr;                        // not owned
            Image2D *accumulatorImage = nullptr;                   // not owned
            Buffer *readbackBuffer = nullptr;                      // not owned
            ShaderInputsBuffer *shaderInputs = nullptr;            // not owned
            const StructureLayout *shaderInputsLayout = nullptr;   // not owned
        };

        virtual ~PathTracerBackend() = default;

        // Static property of the backend class; must be valid before
        // initialize() so the façade knows which resources to create.
        virtual PathTracerOutputKind outputKind() const = 0;

        // BackendImage only: the presentable image the backend renders into.
        // Must remain valid until the backend is destroyed.
        virtual Image2D *backendOutputImage() { return nullptr; }

        // CpuPixels only: tightly packed RGBA pixels (RGBA32F when
        // config.hdrOutput, RGBA8 otherwise) of the latest dispatched frame.
        virtual const void *cpuOutputPixels() const { return nullptr; }

        // Copy the latest rendered frame (tightly packed, same format rules
        // as cpuOutputPixels) into `dst`. Only meaningful after a dispatch
        // with wantReadback=true. Returns the number of bytes written.
        virtual size_t readback(void *dst) = 0;

        // Build pipeline, descriptors, command buffer. Called once at PathTracer
        // construction time.
        virtual void initialize(const InitParams &params) = 0;

        // Upload (or replace) material programs on GPU. The CPU-side
        // MaterialProgramBuffer mirror is owned by the façade; this is a write
        // path only. No-op error if config.useMaterialPrograms is false.
        virtual void uploadMaterialPrograms(const MaterialProgramBuffer &programs) = 0;

        // Re-upload only the parameters slice (the animation-mutable values).
        // Called when the host has mutated the parameter pool in-place.
        virtual void uploadMaterialParameters(const MaterialProgramBuffer &programs) = 0;

        // Record + submit one frame against the given scene. Returns GPU
        // execution time in milliseconds. The façade has already uploaded
        // ShaderInputs (camera + render settings) for this frame; the backend
        // is responsible for clearing the accumulator when requested. When
        // `wantReadback` is true the backend must also enqueue a copy of the
        // final output image into the readback buffer so a subsequent
        // PathTracer::readback() observes the current frame; when false the
        // copy is skipped — the live viewport reads tracer->outputImage()
        // straight off the GPU and never touches the readback buffer, so
        // the extra copy + mapForReading stall is pure overhead there.
        virtual double dispatch(const SceneCompiler::CompiledScene &scene,
                                uint32_t accumulatedSampleCount,
                                bool clearAccumulation,
                                bool wantReadback) = 0;
    };

} // namespace tracey
