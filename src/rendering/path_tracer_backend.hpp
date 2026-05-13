#pragma once

#include "../device/device.hpp"
#include "../device/image_2d.hpp"
#include "../device/buffer.hpp"
#include "../scene/scene_compiler.hpp"
#include "../shading/material_program/material_program.hpp"
#include "../ray_tracing/ray_tracing_pipeline/data_structure.hpp"
#include "../ray_tracing/shader_inputs_buffer.hpp"

#include <memory>

namespace tracey
{
    struct PathTracerConfig;

    // Abstract backend that owns the renderer-side pipeline (compute wavefront,
    // hardware ray tracing, etc). The PathTracer façade owns format-agnostic
    // state (output image, accumulator, sample counter, ShaderInputs uniform)
    // and delegates per-frame dispatch to the backend.
    class PathTracerBackend
    {
    public:
        struct InitParams
        {
            Device *device = nullptr;                              // not owned
            const PathTracerConfig *config = nullptr;              // not owned
            Image2D *outputImage = nullptr;                        // not owned
            Image2D *accumulatorImage = nullptr;                   // not owned
            Buffer *readbackBuffer = nullptr;                      // not owned
            ShaderInputsBuffer *shaderInputs = nullptr;            // not owned
            const StructureLayout *shaderInputsLayout = nullptr;   // not owned
        };

        virtual ~PathTracerBackend() = default;

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

    // Factory: pick a backend based on the device. Today returns
    // WavefrontComputeBackend unconditionally; an RTX path will plug in here.
    std::unique_ptr<PathTracerBackend> selectPathTracerBackend(Device *device);
} // namespace tracey
