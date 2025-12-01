#pragma once
#include "../../core/types.hpp"
#include <random>

namespace tracey
{
    struct RNG
    {
        std::mt19937 rng;
        std::uniform_real_distribution<float> dist;

        RNG(uint32_t seed = 1337u)
            : rng(seed), dist(0.0f, 1.0f)
        {
        }

        float next()
        {
            return dist(rng);
        }

        Vec2 next2D()
        {
            return Vec2{next(), next()};
        }
    };

    Vec3 uniformSampleHemisphere(const Vec2 &xi);
    Vec3 cosineSampleHemisphere(const Vec2 &xi);
} // namespace tracey