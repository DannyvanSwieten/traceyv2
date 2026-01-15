// Simplified PBR BRDF for wavefront path tracing
// Cook-Torrance microfacet model with GGX distribution
// Easier to integrate than full Disney BRDF

#ifndef SIMPLE_PBR_GLSL
#define SIMPLE_PBR_GLSL

#define PI 3.14159265359
#define INV_PI 0.31830988618

// -----------------------------------------------------------------------------
// Simple PBR Material
// -----------------------------------------------------------------------------

struct PBRMaterial {
    vec3 albedo;      // Base color
    float metallic;   // 0 = dielectric, 1 = metal
    float roughness;  // Surface roughness [0,1]
};

PBRMaterial createPBRMaterial(vec3 albedo, float metallic, float roughness) {
    PBRMaterial mat;
    mat.albedo = albedo;
    mat.metallic = metallic;
    mat.roughness = roughness;
    return mat;
}

// -----------------------------------------------------------------------------
// Utility Functions
// -----------------------------------------------------------------------------

float saturate(float x) {
    return clamp(x, 0.0, 1.0);
}

vec3 saturateVec3(vec3 v) {
    return clamp(v, vec3(0.0), vec3(1.0));
}

float sqr(float x) {
    return x * x;
}

// -----------------------------------------------------------------------------
// Fresnel (Schlick Approximation)
// -----------------------------------------------------------------------------

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    float m = saturate(1.0 - cosTheta);
    float m2 = m * m;
    float m5 = m2 * m2 * m;
    return F0 + (vec3(1.0) - F0) * m5;
}

// -----------------------------------------------------------------------------
// GGX Distribution
// -----------------------------------------------------------------------------

float D_GGX(float NdotH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = sqr(NdotH) * (alpha2 - 1.0) + 1.0;
    return alpha2 / (PI * sqr(denom) + 1e-7);
}

// -----------------------------------------------------------------------------
// Smith Geometry Function (GGX)
// -----------------------------------------------------------------------------

float G_SchlickGGX(float NdotX, float roughness) {
    float k = sqr(roughness + 1.0) / 8.0; // For direct lighting
    return NdotX / (NdotX * (1.0 - k) + k + 1e-7);
}

float G_Smith(float NdotL, float NdotV, float roughness) {
    return G_SchlickGGX(NdotL, roughness) * G_SchlickGGX(NdotV, roughness);
}

// -----------------------------------------------------------------------------
// Cook-Torrance Specular BRDF
// -----------------------------------------------------------------------------

vec3 specularBRDF(vec3 N, vec3 V, vec3 L, vec3 F0, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    if (NdotL <= 0.0 || NdotV <= 0.0) {
        return vec3(0.0);
    }

    float D = D_GGX(NdotH, roughness);
    float G = G_Smith(NdotL, NdotV, roughness);
    vec3 F = fresnelSchlick(VdotH, F0);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotL * NdotV + 1e-7;

    return numerator / denominator;
}

// -----------------------------------------------------------------------------
// Lambert Diffuse BRDF
// -----------------------------------------------------------------------------

vec3 lambertBRDF(vec3 albedo) {
    return albedo * INV_PI;
}

// -----------------------------------------------------------------------------
// Combined PBR BRDF
// -----------------------------------------------------------------------------

vec3 evaluatePBR(vec3 N, vec3 V, vec3 L, PBRMaterial mat) {
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));

    if (NdotL <= 0.0 || NdotV <= 0.0) {
        return vec3(0.0);
    }

    // Calculate F0 (base reflectance at normal incidence)
    // Dielectrics: ~0.04, Metals: use albedo
    vec3 F0 = mix(vec3(0.04), mat.albedo, mat.metallic);

    // Specular component
    vec3 F = fresnelSchlick(NdotV, F0);
    vec3 specular = specularBRDF(N, V, L, F0, mat.roughness);

    // Diffuse component (energy-conserving)
    vec3 kD = (vec3(1.0) - F) * (1.0 - mat.metallic);
    vec3 diffuse = kD * lambertBRDF(mat.albedo);

    return diffuse + specular;
}

// -----------------------------------------------------------------------------
// Importance Sampling - GGX
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Importance Sampling - Cosine Hemisphere
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// PDF Calculations
// -----------------------------------------------------------------------------

float pdfGGX(float NdotH, float VdotH, float roughness) {
    float D = D_GGX(NdotH, roughness);
    return (D * NdotH) / (4.0 * VdotH + 1e-7);
}

float pdfCosineHemisphere(float NdotL) {
    return NdotL * INV_PI;
}

// -----------------------------------------------------------------------------
// Build Tangent Frame
// -----------------------------------------------------------------------------

void buildTangentFrame(vec3 N, out vec3 T, out vec3 B) {
    vec3 up = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

vec3 tangentToWorld(vec3 v, vec3 N, vec3 T, vec3 B) {
    return v.x * T + v.y * B + v.z * N;
}

// -----------------------------------------------------------------------------
// Sample PBR BRDF with Importance Sampling
// -----------------------------------------------------------------------------

struct BRDFSample {
    vec3 direction;  // Sampled direction
    vec3 f;          // BRDF value
    float pdf;       // Probability density
};

BRDFSample samplePBR(vec3 N, vec3 V, PBRMaterial mat, float r1, float r2, float r3) {
    BRDFSample result;

    // Build tangent frame
    vec3 T, B;
    buildTangentFrame(N, T, B);

    float NdotV = saturate(dot(N, V));
    vec3 F0 = mix(vec3(0.04), mat.albedo, mat.metallic);
    vec3 F = fresnelSchlick(NdotV, F0);

    // Calculate weights for sampling lobe selection
    float specularWeight = (F.r + F.g + F.b) / 3.0;
    float diffuseWeight = (1.0 - mat.metallic);
    float totalWeight = specularWeight + diffuseWeight;

    // Choose between diffuse and specular sampling
    if (r3 * totalWeight < diffuseWeight) {
        // Sample diffuse lobe (cosine-weighted hemisphere)
        vec3 localDir = sampleCosineHemisphere(r1, r2);
        vec3 L = tangentToWorld(localDir, N, T, B);

        result.direction = L;
        result.f = evaluatePBR(N, V, L, mat);
        result.pdf = pdfCosineHemisphere(saturate(dot(N, L)));
    } else {
        // Sample specular lobe (GGX)
        vec3 H_local = sampleGGX(mat.roughness, r1, r2);
        vec3 H = tangentToWorld(H_local, N, T, B);
        vec3 L = reflect(-V, H);

        float NdotL = saturate(dot(N, L));
        if (NdotL <= 0.0) {
            result.direction = vec3(0.0);
            result.f = vec3(0.0);
            result.pdf = 0.0;
            return result;
        }

        result.direction = L;
        result.f = evaluatePBR(N, V, L, mat);

        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
        result.pdf = pdfGGX(NdotH, VdotH, mat.roughness);
    }

    return result;
}

#endif // SIMPLE_PBR_GLSL
