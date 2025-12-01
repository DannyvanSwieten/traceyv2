#pragma once
#include "pbr.hpp"
namespace tracey
{
    struct RNG;

    struct Sample
    {
        Vec3 wi;   // sampled direction (world space, toward next bounce)
        Vec3 f;    // BSDF value f(N,V,wi)
        float pdf; // pdf(wi | V)
        bool specular;
    };

    // GGX half-vector sampling in local space (z up = N)
    Vec3 sampleGGX_H(const Vec2 &u, float alpha);

    // Sample GGX lobe: returns wi in world space
    Sample sampleGGX(const Vec3 &N, const Vec3 &V, RNG &rng, const PBRMaterial &mat);

    // Sample Lambert diffuse with cosine-weighted hemisphere
    Sample sampleDiffuse(const Vec3 &N, const Vec3 &V, RNG &rng, const PBRMaterial &mat);

    // Mixture sampling: choose between diffuse and specular based on material properties
    Sample sampleBRDF(const Vec3 &N,
                      const Vec3 &V,
                      RNG &rng,
                      const PBRMaterial &mat);
}