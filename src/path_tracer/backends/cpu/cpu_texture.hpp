// CPU texture sampling for the CPU path tracer backend. Replicates the
// GPU sampling the other backends get from hardware: per-texel sRGB→linear
// decode BEFORE bilinear filtering, repeat/clamp address modes, and
// nearest/linear filters — selected by the same 2-bit SamplerKind packed
// into GPUMaterial::samplerBits.

#pragma once

#include "scene/scene_compiler.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace tracey
{
    class CpuTexture
    {
    public:
        explicit CpuTexture(const SceneCompiler::CompiledScene::TextureSource &src);

        // kind: 0 = linear+repeat, 1 = linear+clamp,
        //       2 = nearest+repeat, 3 = nearest+clamp.
        glm::vec4 sample(glm::vec2 uv, uint32_t kind) const;

    private:
        glm::vec4 fetch(int x, int y, bool repeat) const;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        // Linear-space float texels (sRGB decoded at load when the source
        // was colour data — matches the GPU's sRGB view formats).
        std::vector<glm::vec4> m_texels;
    };
} // namespace tracey
