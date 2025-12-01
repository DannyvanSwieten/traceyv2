#pragma once
#include "../../../core/types.hpp"
#include <algorithm> // std::max

namespace tracey
{
    struct PBRMaterial
    {
        Vec3 albedo = Vec3(0.5f);         // Base color
        float metallic = 0.0f;            // 0 = dielectric, 1 = metal
        float roughness = 1.0f;           // Surface roughness
        float sheen = 0.0f;               // Cloth-like sheen
        float clearcoat = 0.0f;           // Clear coat layer
        float clearcoatRoughness = 0.03f; // Clear coat roughness
    };

    // -----------------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------------

    inline float luminance(const Vec3 &c)
    {
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    }

    // -----------------------------------------------------------------------------
    // Fresnel (Schlick approximation)
    // -----------------------------------------------------------------------------

    // F0 is base reflectance at normal incidence (usually metal color or ~0.04 for dielectrics)
    inline Vec3 fresnelSchlick(float cosTheta, const Vec3 &F0)
    {
        cosTheta = saturate(cosTheta);
        return F0 + (Vec3(1.0f) - F0) * glm::pow(1.0f - cosTheta, 5.0f);
    }

    // Slightly roughness-modified Fresnel (optional, often used in “real-time” PBR)
    inline Vec3 fresnelSchlickRoughness(float cosTheta, const Vec3 &F0, float roughness)
    {
        cosTheta = saturate(cosTheta);
        Vec3 oneMinusF0 = Vec3(1.0f) - F0;
        Vec3 F90 = Vec3(1.0f); // at grazing angle
        Vec3 Fr = F0 + (F90 - F0) * glm::pow(1.0f - cosTheta, 5.0f);
        // Sometimes people remap F0 toward 0.04 for rough surfaces; tweak if you like.
        return Fr;
    }

    // -----------------------------------------------------------------------------
    // Lambertian diffuse
    // -----------------------------------------------------------------------------

    // Ideal Lambert BRDF = albedo / π
    inline Vec3 lambert(const Vec3 &albedo)
    {
        constexpr float invPi = 1.0f / 3.1415926535f;
        return albedo * invPi;
    }

    // Cosine-weighted hemisphere pdf wrt solid angle
    inline float pdfCosineHemisphere(float NdotL)
    {
        // pdf = cos(theta) / π
        constexpr float invPi = 1.0f / 3.1415926535f;
        NdotL = saturate(NdotL);
        return NdotL * invPi;
    }

    // -----------------------------------------------------------------------------
    // GGX / Trowbridge-Reitz microfacet
    // -----------------------------------------------------------------------------

    // Normal distribution function (NDF) D_GGX
    // alpha = roughness^2 (common convention)
    inline float D_GGX(float NdotH, float alpha)
    {
        NdotH = saturate(NdotH);
        float a2 = alpha * alpha;
        float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
        denom = 3.1415926535f * denom * denom;
        return a2 / tracey::max(denom, 1e-7f);
    }

    // Geometry term G_Smith using Schlick-GGX
    inline float G_SchlickGGX(float NdotX, float k)
    {
        // k = (alpha + 1)^2 / 8  (for direct lighting)
        NdotX = saturate(NdotX);
        return NdotX / (NdotX * (1.0f - k) + k);
    }

    // Smith masking-shadowing for GGX
    inline float G_Smith(float NdotV, float NdotL, float k)
    {
        return G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);
    }

    // GGX pdf for sampling the reflection direction via half-vector sampling.
    // pdf(ω_o → ω_i) = D(h) * N⋅h / (4 (ω_o⋅h))
    // assuming importance sampling by D.
    inline float pdfGGX(float NdotH, float VdotH, float alpha)
    {
        float D = D_GGX(NdotH, alpha);
        float pdf = (D * NdotH) / std::max(4.0f * VdotH, 1e-7f);
        return pdf;
    }

    // -----------------------------------------------------------------------------
    // BRDF evaluation
    // -----------------------------------------------------------------------------

    // Evaluate Cook-Torrance microfacet specular BRDF with GGX
    // N = surface normal
    // V = view direction (pointing from surface to camera)
    // L = light direction (pointing from surface to light)
    // F0 = base reflectance at normal incidence
    // roughness in [0,1], alpha = roughness^2
    inline Vec3 evalSpecularGGX(const Vec3 &N,
                                const Vec3 &V,
                                const Vec3 &L,
                                const Vec3 &F0,
                                float roughness)
    {
        Vec3 H = tracey::normalize(V + L);
        float NdotL = tracey::dot(N, L);
        float NdotV = tracey::dot(N, V);
        float NdotH = tracey::dot(N, H);
        float VdotH = tracey::dot(V, H);

        if (NdotL <= 0.0f || NdotV <= 0.0f)
        {
            return Vec3(0.0f);
        }

        float alpha = roughness * roughness;

        float D = D_GGX(NdotH, alpha);
        float k = (roughness + 1.0f);
        k = (k * k) / 8.0f; // direct-lighting version
        float G = G_Smith(NdotV, NdotL, k);
        Vec3 F = fresnelSchlick(VdotH, F0);

        float denom = 4.0f * std::max(NdotV, 1e-7f) * std::max(NdotL, 1e-7f);
        return (D * G * F) / std::max(denom, 1e-7f);
    }

    // Evaluate Lambert + GGX “Disney-style” split
    // albedo: base color
    // metallic: 0 = dielectric, 1 = full metal
    inline Vec3 evalBRDF(const Vec3 &N,
                         const Vec3 &V,
                         const Vec3 &L,
                         const Vec3 &albedo,
                         float metallic,
                         float roughness)
    {
        float NdotL = tracey::dot(N, L);
        float NdotV = tracey::dot(N, V);
        if (NdotL <= 0.0f || NdotV <= 0.0f)
        {
            return Vec3(0.0f);
        }

        // F0: mix between base 0.04 and albedo for metals
        Vec3 F0 = tracey::mix(Vec3(0.04f), albedo, metallic);

        Vec3 F_spec = evalSpecularGGX(N, V, L, F0, roughness);

        // Energy-conserving diffuse: scaled by (1 - F) and (1 - metallic)
        Vec3 F = fresnelSchlick(tracey::dot(N, V), F0);
        Vec3 kd = (Vec3(1.0f) - F) * (1.0f - metallic);
        Vec3 diffuse = kd * lambert(albedo); // albedo/π

        return diffuse + F_spec;
    }

    // -----------------------------------------------------------------------------
    // PDFs for mixed sampling (diffuse + GGX)
    // -----------------------------------------------------------------------------

    // Diffuse pdf for a cosine-weighted sample:
    // pdf_diffuse = cos(theta) / π
    inline float pdfDiffuse(const Vec3 &N, const Vec3 &L)
    {
        float NdotL = tracey::dot(N, L);
        return pdfCosineHemisphere(NdotL);
    }

    // Combined pdf for mixture sampling: w_diff * pdf_diffuse + w_spec * pdf_spec
    inline float pdfBrdf(const Vec3 &N, const Vec3 &V, const Vec3 &L,
                         float roughness,
                         float weightDiffuse,
                         float weightSpecular)
    {
        Vec3 H = tracey::normalize(V + L);
        float NdotH = tracey::dot(N, H);
        float VdotH = tracey::dot(V, H);

        float pdfD = pdfDiffuse(N, L);
        float alpha = roughness * roughness;
        float pdfS = pdfGGX(NdotH, VdotH, alpha);

        return weightDiffuse * pdfD + weightSpecular * pdfS;
    }

} // namespace tracey::pbr