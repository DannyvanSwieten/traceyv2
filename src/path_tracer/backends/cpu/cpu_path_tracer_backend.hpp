// Native CPU path tracer backend — the universal fallback. A C++
// translation of the same megakernel the Metal backend runs (which is in
// turn a line-by-line port of the canonical GLSL set), traversing the
// engine's own BVH (core Blas/Tlas via cpuBlas()) and interpreting the
// packed MaterialProgram bytecode directly. RNG and seeding are bit-exact
// with the GPU backends so pt_backend_compare can gate parity.
//
// Output contract: PathTracerOutputKind::CpuPixels — the façade uploads
// cpuOutputPixels() into its Vulkan image for the viewport compositor;
// readback() serves exports straight from host memory.

#pragma once

#include "path_tracer/api/path_tracer_backend.hpp"
#include "cpu_texture.hpp"

#include "core/tlas.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace tracey
{
    class CpuPathTracerBackend : public PathTracerBackend
    {
    public:
        void initialize(const InitParams &params) override;
        PathTracerOutputKind outputKind() const override
        {
            return PathTracerOutputKind::CpuPixels;
        }
        const void *cpuOutputPixels() const override { return m_pixels.data(); }
        void uploadMaterialPrograms(const MaterialProgramBuffer &programs) override;
        void uploadMaterialParameters(const MaterialProgramBuffer &programs) override;
        double dispatch(const SceneCompiler::CompiledScene &scene,
                        uint32_t accumulatedSampleCount,
                        bool clearAccumulation,
                        bool wantReadback) override;
        size_t readback(void *dst) override;

    private:
        void bindScene(const SceneCompiler::CompiledScene &scene);

        const PathTracerConfig *m_config = nullptr;
        ShaderInputsBuffer *m_shaderInputs = nullptr;

        // Frame state.
        std::vector<glm::vec4> m_accumulator;  // running mean per pixel
        std::vector<uint8_t> m_pixels;         // packed RGBA8 or RGBA32F

        // Per-scene state, rebuilt when revision changes.
        uint64_t m_sceneRevision = ~0ull;
        std::vector<const Blas *> m_blasPtrs;
        std::vector<Tlas::Instance> m_instances;
        std::unique_ptr<Tlas> m_tlas;
        std::vector<GPULight> m_lights;
        std::vector<GPUMaterial> m_materials;       // per-instance
        std::vector<glm::uvec2> m_instanceData;     // programId, uvOffset
        std::vector<glm::vec2> m_uvs;               // global per-vertex
        std::vector<glm::vec4> m_normals;           // global per-vertex
        std::vector<CpuTexture> m_textures;

        // Packed material programs (interpreted directly).
        MaterialProgramBuffer m_programs;
    };
} // namespace tracey
