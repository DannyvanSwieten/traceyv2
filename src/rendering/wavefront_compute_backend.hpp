#pragma once

#include "path_tracer_backend.hpp"

#include "../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline.hpp"
#include "../ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../ray_tracing/ray_tracing_pipeline/descriptor_set.hpp"
#include "../ray_tracing/ray_tracing_command_buffer/ray_tracing_command_buffer.hpp"
#include "../ray_tracing/shader_builder/ray_tracing_shader_builder.hpp"

#include <array>
#include <memory>

namespace tracey
{
    // Wavefront-compute backend: today's path tracer pipeline (ray-gen →
    // intersect → hit/miss queues → resolve, looped per bounce on the compute
    // queue). Owns the pipeline, descriptors, command buffer, and the four
    // MaterialProgram SSBOs.
    class WavefrontComputeBackend : public PathTracerBackend
    {
    public:
        WavefrontComputeBackend();
        ~WavefrontComputeBackend() override;

        void initialize(const InitParams &params) override;
        void uploadMaterialPrograms(const MaterialProgramBuffer &programs) override;
        void uploadMaterialParameters(const MaterialProgramBuffer &programs) override;
        double dispatch(const SceneCompiler::CompiledScene &scene,
                        uint32_t accumulatedSampleCount,
                        bool clearAccumulation,
                        bool wantReadback) override;

    private:
        void buildPipeline();
        void allocateDescriptorSets();
        void bindSceneResources(const SceneCompiler::CompiledScene &scene);

        // Pointers to façade-owned state. Captured during initialize().
        Device *m_device = nullptr;
        const PathTracerConfig *m_config = nullptr;
        Image2D *m_outputImage = nullptr;
        Image2D *m_accumulatorImage = nullptr;
        Buffer *m_readbackBuffer = nullptr;
        ShaderInputsBuffer *m_shaderInputs = nullptr;
        const StructureLayout *m_shaderInputsLayout = nullptr;

        // Pipeline / dispatch state owned by this backend.
        std::unique_ptr<RayTracingShaderBuilder> m_pipelineBuilder;
        std::unique_ptr<RayTracingPipelineLayoutDescriptor> m_pipelineLayout;
        std::unique_ptr<RayTracingPipeline> m_pipeline;
        std::array<std::unique_ptr<DescriptorSet>, 2> m_descriptorSets;
        std::unique_ptr<RayTracingCommandBuffer> m_commandBuffer;

        // Material program SSBOs (allocated when useMaterialPrograms is true).
        std::unique_ptr<Buffer> m_programCodeBuffer;
        std::unique_ptr<Buffer> m_programConstantsBuffer;
        std::unique_ptr<Buffer> m_programHeadersBuffer;
        std::unique_ptr<Buffer> m_programParametersBuffer;
    };
} // namespace tracey
