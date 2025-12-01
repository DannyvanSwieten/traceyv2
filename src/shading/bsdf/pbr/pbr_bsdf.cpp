#include "pbr_bsdf.hpp"
#include "../../onb.hpp"
#include "../../random/sampling.hpp"
namespace tracey
{
    Vec3 sampleGGX_H(const Vec2 &u, float alpha)
    {
        float a2 = alpha * alpha;

        float phi = 2.0f * tracey::pi<float>() * u.x;
        float cosTheta = std::sqrt((1.0f - u.y) / (1.0f + (a2 - 1.0f) * u.y));
        float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));

        float x = sinTheta * std::cos(phi);
        float y = sinTheta * std::sin(phi);
        float z = cosTheta;

        return Vec3(x, y, z);
    }
    Sample sampleGGX(const Vec3 &N, const Vec3 &V, RNG &rng, const PBRMaterial &mat)
    {
        Sample s{};
        s.specular = true;

        float roughness = clamp(mat.roughness, 0.001f, 1.0f);
        float alpha = roughness * roughness;

        // Build ONB from N for half-vector sampling
        const auto onb = OrthogonalBasis::fromNormal(N);

        // Sample half-vector H in local space, then to world
        Vec2 u = rng.next2D();
        Vec3 H_l = sampleGGX_H(u, alpha);
        Vec3 H = normalize(onb.toWorld(H_l));
        // Reflect V about H to get wi
        Vec3 wi = reflect(-V, H);

        // If under the surface, reject
        if (dot(wi, N) <= 0.0f)
        {
            s.pdf = 0.0f;
            s.f = Vec3(0.0f);
            return s;
        }

        float NdotL = dot(N, wi);
        float NdotV = dot(N, V);
        float NdotH = dot(N, H);
        float VdotH = dot(V, H);

        if (NdotL <= 0.0f || NdotV <= 0.0f)
        {
            s.pdf = 0.0f;
            s.f = Vec3(0.0f);
            return s;
        }

        Vec3 F0 = tracey::mix(Vec3(0.04f), mat.albedo, mat.metallic);

        float D = D_GGX(NdotH, alpha);
        float k = (roughness + 1.0f);
        k = (k * k) / 8.0f;
        float G = G_Smith(NdotV, NdotL, k);
        Vec3 F = fresnelSchlick(VdotH, F0);

        float denom = 4.0f * std::max(NdotV, 1e-7f) * std::max(NdotL, 1e-7f);
        Vec3 f = (D * G * F) / std::max(denom, 1e-7f);

        // PDF for wi when sampling H from GGX
        float pdf_h = D * NdotH;
        float pdf_wi = pdf_h / std::max(4.0f * VdotH, 1e-7f);

        s.wi = tracey::normalize(wi);
        s.f = f;
        s.pdf = pdf_wi;
        return s;
    }
    Sample sampleDiffuse(const Vec3 &N, const Vec3 &V, RNG &rng, const PBRMaterial &mat)
    {
        Sample s{};
        s.specular = false;

        const auto onb = OrthogonalBasis::fromNormal(N);
        Vec3 localDir = tracey::cosineSampleHemisphere(rng.next2D());
        Vec3 wi = tracey::normalize(onb.toWorld(localDir));

        float NdotL = tracey::dot(N, wi);
        if (NdotL <= 0.0f)
        {
            s.pdf = 0.0f;
            s.f = Vec3(0.0f);
            return s;
        }

        Vec3 brdf = lambert(mat.albedo); // albedo / π
        float pdf = pdfCosineHemisphere(NdotL);

        s.wi = wi;
        s.f = brdf;
        s.pdf = pdf;
        return s;
    }
    Sample sampleBRDF(const Vec3 &N, const Vec3 &V, RNG &rng, const PBRMaterial &mat)
    {
        float specWeight = clamp(tracey::luminance(mat.albedo) * mat.metallic, 0.05f, 0.95f);
        float diffuseWeight = 1.0f - specWeight;

        float xi = rng.next(); // uniform [0,1)

        Sample s;
        bool choseSpec = (xi < specWeight);

        if (choseSpec)
        {
            s = sampleGGX(N, V, rng, mat);
            if (s.pdf <= 0.0f)
                return s;

            // Account for mixture pdf: p = w_spec * pdf_spec + w_diff * pdf_diff
            float NdotL = tracey::max(tracey::dot(N, s.wi), 0.0f);
            float pdfDiffuse = pdfCosineHemisphere(NdotL);
            float pdfMixed = specWeight * s.pdf + diffuseWeight * pdfDiffuse;
            s.pdf = pdfMixed;
        }
        else
        {
            s = sampleDiffuse(N, V, rng, mat);
            if (s.pdf <= 0.0f)
                return s;

            // Same mixture pdf
            float roughness = std::clamp(mat.roughness, 0.001f, 1.0f);
            float alpha = roughness * roughness;
            Vec3 H = tracey::normalize(V + s.wi);
            float NdotH = tracey::max(tracey::dot(N, H), 0.0f);
            float VdotH = tracey::max(tracey::dot(V, H), 0.0f);
            float pdfSpec = pdfGGX(NdotH, VdotH, alpha);
            float pdfMixed = diffuseWeight * s.pdf + specWeight * pdfSpec;
            s.pdf = pdfMixed;
        }

        return s;
    }
} // namespace tracey