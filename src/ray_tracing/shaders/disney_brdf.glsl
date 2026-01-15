// Disney-style PBR BRDF implementation for GPU path tracing
// Based on Disney's "Principled BRDF" and Burley's original paper
// https://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_notes_v3.pdf

#ifndef DISNEY_BRDF_GLSL
#define DISNEY_BRDF_GLSL

// Constants
#define PI 3.14159265359
#define INV_PI 0.31830988618
#define EPSILON 1e-7

// -----------------------------------------------------------------------------
// Material Definition
// -----------------------------------------------------------------------------

struct DisneyMaterial {
    vec3 baseColor;           // Base color (albedo)
    float metallic;           // 0 = dielectric, 1 = metal
    float roughness;          // Surface roughness [0,1]
    float subsurface;         // Subsurface scattering amount
    float specular;           // Specular intensity (usually 0.5)
    float specularTint;       // Tint specular toward baseColor
    float anisotropic;        // Anisotropic reflections
    float sheen;              // Cloth-like sheen
    float sheenTint;          // Sheen color tint
    float clearcoat;          // Clear coat layer
    float clearcoatGloss;     // Clear coat glossiness
};

// Default material constructor
DisneyMaterial defaultDisneyMaterial() {
    DisneyMaterial mat;
    mat.baseColor = vec3(0.5);
    mat.metallic = 0.0;
    mat.roughness = 0.5;
    mat.subsurface = 0.0;
    mat.specular = 0.5;
    mat.specularTint = 0.0;
    mat.anisotropic = 0.0;
    mat.sheen = 0.0;
    mat.sheenTint = 0.0;
    mat.clearcoat = 0.0;
    mat.clearcoatGloss = 1.0;
    return mat;
}

// -----------------------------------------------------------------------------
// Utility Functions
// -----------------------------------------------------------------------------

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float sqr(float x) {
    return x * x;
}

vec3 mix3(vec3 a, vec3 b, float t) {
    return a * (1.0 - t) + b * t;
}

// -----------------------------------------------------------------------------
// Fresnel
// -----------------------------------------------------------------------------

// Schlick's approximation
float schlickFresnel(float cosTheta) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    return m * m * m * m * m;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (vec3(1.0) - F0) * schlickFresnel(cosTheta);
}

// Disney's Fresnel for dielectrics
float disneyFresnel(float cosTheta, float eta) {
    float sinThetaT2 = sqr(eta) * (1.0 - sqr(cosTheta));
    if (sinThetaT2 > 1.0) return 1.0; // Total internal reflection

    float cosThetaT = sqrt(max(0.0, 1.0 - sinThetaT2));
    float rs = (cosTheta - eta * cosThetaT) / (cosTheta + eta * cosThetaT);
    float rp = (eta * cosTheta - cosThetaT) / (eta * cosTheta + cosThetaT);
    return 0.5 * (rs * rs + rp * rp);
}

// -----------------------------------------------------------------------------
// GGX/Trowbridge-Reitz Distribution
// -----------------------------------------------------------------------------

// GGX Normal Distribution Function
float D_GGX(float NdotH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = sqr(NdotH * NdotH * (alpha2 - 1.0) + 1.0);
    return alpha2 / (PI * max(denom, EPSILON));
}

// GGX Anisotropic NDF
float D_GGX_Anisotropic(float NdotH, float HdotX, float HdotY, float ax, float ay) {
    float a = HdotX / ax;
    float b = HdotY / ay;
    float c = a * a + b * b + NdotH * NdotH;
    return 1.0 / (PI * ax * ay * sqr(c));
}

// Smith masking-shadowing term for GGX
float smithG_GGX(float NdotV, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    return 2.0 * NdotV / (NdotV + sqrt(alpha2 + (1.0 - alpha2) * NdotV * NdotV));
}

float G_Smith(float NdotL, float NdotV, float roughness) {
    return smithG_GGX(NdotL, roughness) * smithG_GGX(NdotV, roughness);
}

// -----------------------------------------------------------------------------
// Disney Diffuse (Burley)
// -----------------------------------------------------------------------------

float disney_diffuse(float NdotL, float NdotV, float LdotH, float roughness) {
    float fd90 = 0.5 + 2.0 * LdotH * LdotH * roughness;
    float lightScatter = 1.0 + (fd90 - 1.0) * schlickFresnel(NdotL);
    float viewScatter = 1.0 + (fd90 - 1.0) * schlickFresnel(NdotV);
    return lightScatter * viewScatter * INV_PI;
}

// -----------------------------------------------------------------------------
// Disney Sheen
// -----------------------------------------------------------------------------

vec3 disney_sheen(float LdotH, vec3 baseColor, float sheen, float sheenTint) {
    vec3 tint = normalize(baseColor + vec3(EPSILON));
    vec3 sheenColor = mix3(vec3(1.0), tint, sheenTint);
    return sheen * sheenColor * schlickFresnel(LdotH);
}

// -----------------------------------------------------------------------------
// Disney Clearcoat
// -----------------------------------------------------------------------------

float disney_clearcoat(float NdotH, float LdotH, float clearcoat, float clearcoatGloss) {
    // Clearcoat uses a fixed IOR of 1.5 (F0 = 0.04)
    float alpha = mix(0.1, 0.001, clearcoatGloss);
    float Dr = (alpha * alpha - 1.0) / (PI * log(alpha * alpha) * (1.0 + (alpha * alpha - 1.0) * NdotH * NdotH));
    float Fr = schlickFresnel(LdotH);
    float Gr = 0.25 / (LdotH * LdotH); // Simplified geometry term
    return clearcoat * Fr * Dr * Gr;
}

// -----------------------------------------------------------------------------
// Disney BRDF Evaluation
// -----------------------------------------------------------------------------

struct BRDFSample {
    vec3 f;        // BRDF value
    float pdf;     // Probability density
};

// Evaluate full Disney BRDF
vec3 evaluateDisneyBRDF(
    vec3 N, vec3 V, vec3 L, vec3 X, vec3 Y,
    DisneyMaterial mat,
    out float pdf
) {
    float NdotL = dot(N, L);
    float NdotV = dot(N, V);

    if (NdotL <= 0.0 || NdotV <= 0.0) {
        pdf = 0.0;
        return vec3(0.0);
    }

    vec3 H = normalize(V + L);
    float NdotH = dot(N, H);
    float LdotH = dot(L, H);
    float VdotH = dot(V, H);

    // Compute F0 for dielectrics and metals
    float luminance = dot(mat.baseColor, vec3(0.3, 0.6, 0.1));
    vec3 tint = luminance > 0.0 ? mat.baseColor / luminance : vec3(1.0);
    vec3 specTint = mix3(vec3(1.0), tint, mat.specularTint);
    vec3 F0 = mix3(mat.specular * 0.08 * specTint, mat.baseColor, mat.metallic);

    // === Diffuse Component ===
    vec3 diffuse = vec3(0.0);
    if (mat.metallic < 1.0) {
        float Fd = disney_diffuse(NdotL, NdotV, LdotH, mat.roughness);

        // Energy compensation for metallic
        diffuse = (1.0 - mat.metallic) * mat.baseColor * Fd;

        // Add subsurface scattering approximation
        if (mat.subsurface > 0.0) {
            float Fss90 = LdotH * LdotH * mat.roughness;
            float Fss = mix(1.0, Fss90, schlickFresnel(NdotL)) * mix(1.0, Fss90, schlickFresnel(NdotV));
            float ss = 1.25 * (Fss * (1.0 / (NdotL + NdotV) - 0.5) + 0.5);
            diffuse = mix3(diffuse, ss * mat.baseColor * INV_PI, mat.subsurface);
        }
    }

    // === Specular Component ===
    float D = D_GGX(NdotH, mat.roughness);
    float G = G_Smith(NdotL, NdotV, mat.roughness);
    vec3 F = fresnelSchlick(VdotH, F0);

    vec3 specular = (D * G * F) / max(4.0 * NdotL * NdotV, EPSILON);

    // === Sheen Component ===
    vec3 sheen = vec3(0.0);
    if (mat.sheen > 0.0 && mat.metallic < 1.0) {
        sheen = (1.0 - mat.metallic) * disney_sheen(LdotH, mat.baseColor, mat.sheen, mat.sheenTint);
    }

    // === Clearcoat Component ===
    vec3 clearcoat = vec3(0.0);
    if (mat.clearcoat > 0.0) {
        clearcoat = vec3(disney_clearcoat(NdotH, LdotH, mat.clearcoat, mat.clearcoatGloss));
    }

    // === PDF Calculation ===
    // Simplified: cosine-weighted hemisphere for diffuse
    // For production, use importance sampling with proper MIS
    pdf = NdotL * INV_PI;

    return diffuse + specular + sheen + clearcoat;
}

// -----------------------------------------------------------------------------
// Importance Sampling
// -----------------------------------------------------------------------------

// Sample GGX distribution
vec3 sampleGGX(float roughness, float r1, float r2) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;

    float cosTheta = sqrt((1.0 - r2) / (1.0 + (alpha2 - 1.0) * r2));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 2.0 * PI * r1;

    return vec3(
        cos(phi) * sinTheta,
        sin(phi) * sinTheta,
        cosTheta
    );
}

// Sample cosine-weighted hemisphere
vec3 sampleCosineHemisphere(float r1, float r2) {
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt(r2);
    float sinTheta = sqrt(1.0 - r2);

    return vec3(
        cos(phi) * sinTheta,
        sin(phi) * sinTheta,
        cosTheta
    );
}

// Build orthonormal basis from normal
void buildOrthonormalBasis(vec3 N, out vec3 T, out vec3 B) {
    vec3 up = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// Transform from tangent space to world space
vec3 tangentToWorld(vec3 v, vec3 N, vec3 T, vec3 B) {
    return v.x * T + v.y * B + v.z * N;
}

// Sample Disney BRDF with importance sampling
BRDFSample sampleDisneyBRDF(
    vec3 N, vec3 V,
    DisneyMaterial mat,
    float r1, float r2, float r3
) {
    BRDFSample result;

    // Build tangent frame
    vec3 T, B;
    buildOrthonormalBasis(N, T, B);

    // Choose lobe based on material properties
    // Simplified: choose between diffuse and specular
    float specularWeight = luminance(fresnelSchlick(dot(N, V), vec3(0.04)));
    float diffuseWeight = 1.0 - mat.metallic;
    float totalWeight = specularWeight + diffuseWeight;

    vec3 localDir;

    if (r3 * totalWeight < diffuseWeight) {
        // Sample diffuse lobe (cosine-weighted hemisphere)
        localDir = sampleCosineHemisphere(r1, r2);
    } else {
        // Sample specular lobe (GGX)
        vec3 H_local = sampleGGX(mat.roughness, r1, r2);
        vec3 H = tangentToWorld(H_local, N, T, B);
        vec3 L = reflect(-V, H);

        if (dot(N, L) <= 0.0) {
            result.f = vec3(0.0);
            result.pdf = 0.0;
            return result;
        }

        result.f = evaluateDisneyBRDF(N, V, L, T, B, mat, result.pdf);
        return result;
    }

    vec3 L = tangentToWorld(localDir, N, T, B);
    result.f = evaluateDisneyBRDF(N, V, L, T, B, mat, result.pdf);

    return result;
}

// -----------------------------------------------------------------------------
// Simplified Lambert BRDF (for backward compatibility)
// -----------------------------------------------------------------------------

vec3 lambertBRDF(vec3 albedo) {
    return albedo * INV_PI;
}

float lambertPDF(float NdotL) {
    return NdotL * INV_PI;
}

#endif // DISNEY_BRDF_GLSL
