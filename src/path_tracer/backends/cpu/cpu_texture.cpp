#include "cpu_texture.hpp"

#include <algorithm>
#include <cmath>

namespace tracey
{
    namespace
    {
        // IEC 61966-2-1 sRGB EOTF — the same decode the GPU's sRGB view
        // formats apply per texel before filtering.
        float srgbToLinear(float c)
        {
            return c <= 0.04045f ? c / 12.92f
                                 : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
    }

    CpuTexture::CpuTexture(const SceneCompiler::CompiledScene::TextureSource &src)
        : m_width(src.width), m_height(src.height)
    {
        const size_t count = static_cast<size_t>(src.width) * src.height;
        m_texels.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            glm::vec4 t(src.rgba8[i * 4 + 0] / 255.0f,
                        src.rgba8[i * 4 + 1] / 255.0f,
                        src.rgba8[i * 4 + 2] / 255.0f,
                        src.rgba8[i * 4 + 3] / 255.0f);
            if (src.srgb)
            {
                t.r = srgbToLinear(t.r);
                t.g = srgbToLinear(t.g);
                t.b = srgbToLinear(t.b);
                // Alpha stays linear, per the sRGB texture rules.
            }
            m_texels[i] = t;
        }
    }

    glm::vec4 CpuTexture::fetch(int x, int y, bool repeat) const
    {
        const int w = static_cast<int>(m_width);
        const int h = static_cast<int>(m_height);
        if (repeat)
        {
            x = ((x % w) + w) % w;
            y = ((y % h) + h) % h;
        }
        else
        {
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
        }
        return m_texels[static_cast<size_t>(y) * m_width + x];
    }

    glm::vec4 CpuTexture::sample(glm::vec2 uv, uint32_t kind) const
    {
        if (m_width == 0 || m_height == 0) return glm::vec4(1.0f);
        const bool repeat = (kind == 0u || kind == 2u);
        const bool nearest = (kind >= 2u);

        // GL/Metal texel addressing: sample point at uv*size, texel centres
        // at integer+0.5.
        const float fx = uv.x * static_cast<float>(m_width) - 0.5f;
        const float fy = uv.y * static_cast<float>(m_height) - 0.5f;

        if (nearest)
        {
            return fetch(static_cast<int>(std::round(fx)),
                         static_cast<int>(std::round(fy)), repeat);
        }

        const int x0 = static_cast<int>(std::floor(fx));
        const int y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);

        const glm::vec4 c00 = fetch(x0, y0, repeat);
        const glm::vec4 c10 = fetch(x0 + 1, y0, repeat);
        const glm::vec4 c01 = fetch(x0, y0 + 1, repeat);
        const glm::vec4 c11 = fetch(x0 + 1, y0 + 1, repeat);

        return glm::mix(glm::mix(c00, c10, tx), glm::mix(c01, c11, tx), ty);
    }
} // namespace tracey
