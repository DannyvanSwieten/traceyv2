#pragma once
#include "../../core/types.hpp"
#include <random>

#ifdef _WIN32
#pragma warning(push)
#pragma warning( disable : 4146 ) // unary minus operator applied to unsigned type, result still unsigned
#endif

namespace tracey
{
    class RNG
    {
    public:
        virtual float next() = 0;
        virtual Vec2 next2D() = 0;
    };
    class STDRNG : public RNG
    {
        std::mt19937 rng;
        std::uniform_real_distribution<float> dist;

    public:
        STDRNG(uint32_t seed = 1337u)
            : rng(seed), dist(0.0f, 1.0f)
        {
        }

        float next() override
        {
            return dist(rng);
        }

        Vec2 next2D() override
        {
            return Vec2{next(), next()};
        }
    };

    class SimpleRNG : public RNG
    {
        uint32_t state;

    public:
        SimpleRNG(uint32_t seed = 1337u)
            : state(seed)
        {
        }

        float next() override
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return static_cast<float>(state) / static_cast<float>(0xFFFFFFFF);
        }

        Vec2 next2D() override
        {
            return Vec2{next(), next()};
        }
    };

    class PCG32 : public RNG
    {
        uint64_t state = 0x853c49e6748fea9bULL;
        uint64_t inc = 0xda3e39cb94b95bdbULL;

        uint32_t nextUInt()
        {
            uint64_t oldstate = state;
            // Advance internal state
            state = oldstate * 6364136223846793005ULL + (inc | 1);

            // Calculate output function (XSH RR)
            uint32_t xorshifted = static_cast<uint32_t>(((oldstate >> 18u) ^ oldstate) >> 27u);
            uint32_t rot = static_cast<uint32_t>(oldstate >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
        }

    public:
        PCG32(uint32_t seed = 1337u)
        {
            state += seed;
            nextUInt();
        }
        float next() override
        {
            // Convert to float in [0,1)
            return (nextUInt() >> 8) * (1.0f / 16777216.0f); // 2^24
        }

        Vec2 next2D() override
        {
            return Vec2{next(), next()};
        }
    };

    Vec3 uniformSampleHemisphere(const Vec2 &xi);
    Vec3 cosineSampleHemisphere(const Vec2 &xi);
} // namespace tracey

#ifdef _WIN32
#pragma warning(pop)
#endif