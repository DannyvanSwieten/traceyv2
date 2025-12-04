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
        float specWeight    = clamp(tracey::luminance(mat.albedo) * mat.metallic, 0.05f, 0.95f);
        float diffuseWeight = 1.0f - specWeight;

        float xi = rng.next(); // uniform [0,1)

        Sample s{};
        bool choseSpec = (xi < specWeight);

        if (choseSpec)
        {
            // 1) Sample specular lobe
            s = sampleGGX(N, V, rng, mat);
            if (s.pdf <= 0.0f)
                return s;

            const Vec3 wi    = s.wi;
            const float NdotL = tracey::max(tracey::dot(N, wi), 0.0f);
            if (NdotL <= 0.0f)
            {
                s.pdf = 0.0f;
                s.f   = Vec3(0.0f);
                return s;
            }

            // Evaluate diffuse BRDF at this wi
            Vec3 f_diff = lambert(mat.albedo);           // or (1 - mat.metallic) * lambert(...)
            float pdfDiffuse = pdfCosineHemisphere(NdotL);

            // Mixture pdf
            float pdfMixed = specWeight * s.pdf + diffuseWeight * pdfDiffuse;

            // Full BRDF = spec + diffuse
            s.f   = s.f + f_diff;
            s.pdf = pdfMixed;
        }
        else
        {
            // 2) Sample diffuse lobe
            s = sampleDiffuse(N, V, rng, mat);
            if (s.pdf <= 0.0f)
                return s;

            const Vec3 wi    = s.wi;
            const float NdotL = tracey::max(tracey::dot(N, wi), 0.0f);
            if (NdotL <= 0.0f)
            {
                s.pdf = 0.0f;
                s.f   = Vec3(0.0f);
                return s;
            }

            // Evaluate spec GGX at this wi
            float roughness = std::clamp(mat.roughness, 0.001f, 1.0f);
            float alpha = roughness * roughness;
            Vec3 H = tracey::normalize(V + wi);

            float NdotH = tracey::max(tracey::dot(N, H), 0.0f);
            float VdotH = tracey::max(tracey::dot(V, H), 0.0f);
            float NdotV = tracey::max(tracey::dot(N, V), 0.0f);

            // Same GGX eval you use in sampleGGX:
            Vec3 F0 = tracey::mix(Vec3(0.04f), mat.albedo, mat.metallic);
            float D = D_GGX(NdotH, alpha);
            float k = (roughness + 1.0f);
            k = (k * k) / 8.0f;
            float G = G_Smith(NdotV, NdotL, k); // NdotV known or recompute
            Vec3 F = fresnelSchlick(VdotH, F0);
            float denom = 4.0f * glm::max(NdotV, 1e-7f) * glm::max(NdotL, 1e-7f);
            Vec3 f_spec = (D * G * F) / std::max(denom, 1e-7f);

            float pdfSpec = pdfGGX(NdotH, VdotH, alpha);

            float pdfMixed = diffuseWeight * s.pdf + specWeight * pdfSpec;

            s.f   = f_spec + s.f;   // s.f already has diffuse from sampleDiffuse
            s.pdf = pdfMixed;
        }

        return s;
    }

    Vec3 evalBRDF(const Vec3 &N, const Vec3 &V, const Vec3 &L, const PBRMaterial &mat)
    {
        float NdotL = tracey::max(tracey::dot(N, L), 0.0f);
        float NdotV = tracey::max(tracey::dot(N, V), 0.0f);
        if (NdotL <= 0.0f || NdotV <= 0.0f)
            return Vec3(0.0f);

        Vec3 H = tracey::normalize(V + L);
        float NdotH = tracey::max(tracey::dot(N, H), 0.0f);
        float VdotH = tracey::max(tracey::dot(V, H), 0.0f);

        float roughness = clamp(mat.roughness, 0.001f, 1.0f);
        float alpha     = roughness * roughness;

        // Specular GGX
        Vec3 F0 = tracey::mix(Vec3(0.04f), mat.albedo, mat.metallic);
        float D = D_GGX(NdotH, alpha);
        float k = (roughness + 1.0f);
        k       = (k * k) / 8.0f;
        float G = G_Smith(NdotV, NdotL, k);
        Vec3 F  = fresnelSchlick(VdotH, F0);

        float denom = 4.0f * std::max(NdotV, 1e-7f) * std::max(NdotL, 1e-7f);
        Vec3 f_spec = (D * G * F) / std::max(denom, 1e-7f);

        // Diffuse (you can decide if you want (1 - metallic) here)
        Vec3 f_diff = lambert(mat.albedo);

        return f_spec + f_diff;
    }

    float pdfBRDF(const Vec3 &N, const Vec3 &V, const Vec3 &L, const PBRMaterial &mat)
    {
        float NdotL = tracey::max(tracey::dot(N, L), 0.0f);
        if (NdotL <= 0.0f)
            return 0.0f;

        float specWeight    = clamp(tracey::luminance(mat.albedo) * mat.metallic, 0.05f, 0.95f);
        float diffuseWeight = 1.0f - specWeight;

        // Diffuse pdf
        float pdfDiffuse = pdfCosineHemisphere(NdotL);

        // Spec pdf (must match your GGX sampling)
        float roughness = clamp(mat.roughness, 0.001f, 1.0f);
        float alpha     = roughness * roughness;
        Vec3 H          = tracey::normalize(V + L);
        float NdotH     = tracey::max(tracey::dot(N, H), 0.0f);
        float VdotH     = tracey::max(tracey::dot(V, H), 0.0f);

        float pdfSpec = pdfGGX(NdotH, VdotH, alpha);

        return specWeight * pdfSpec + diffuseWeight * pdfDiffuse;
    }
} // namespace tracey