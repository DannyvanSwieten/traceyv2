// PBR Library for ISF shaders
// Common functions and utilities for physically-based rendering

#ifndef PBR_LIB_GLSL
#define PBR_LIB_GLSL

#define PI 3.14159265359

// ============================================================================
// Material Data Access
// ============================================================================

// GPUMaterial layout (80 bytes = 20 ints):
// offset 0:  albedoTexIndex (int)
// offset 1:  normalTexIndex (int)
// offset 2:  metallicRoughnessTexIndex (int)
// offset 3:  emissiveTexIndex (int)
// offset 4:  occlusionTexIndex (int)
// offset 5-7: padding
// offset 8:  baseColorR (float)
// offset 9:  baseColorG (float)
// offset 10: baseColorB (float)
// offset 11: baseColorA (float)
// offset 12: metallicFactor (float)
// offset 13: roughnessFactor (float)
// offset 14: emissiveR (float)
// offset 15: emissiveG (float)
// offset 16: emissiveB (float)
// offset 17-19: padding
#define MATERIAL_STRIDE 20

// Get albedo color from material (texture or base color factor)
vec3 getMaterialAlbedo(uint instanceIndex, vec2 uv) {
    uint baseOffset = instanceIndex * uint(MATERIAL_STRIDE);
    int albedoTexIdx = materials.data[baseOffset + 0u];

    float baseR = intBitsToFloat(materials.data[baseOffset + 8u]);
    float baseG = intBitsToFloat(materials.data[baseOffset + 9u]);
    float baseB = intBitsToFloat(materials.data[baseOffset + 10u]);
    vec3 baseColor = vec3(baseR, baseG, baseB);

    if (albedoTexIdx >= 0) {
        vec4 texColor = texture(sampler2D(textures[albedoTexIdx], linearSampler), uv);
        return texColor.rgb * baseColor;
    }

    return baseColor;
}

// Get metallic and roughness from material
vec2 getMaterialMetallicRoughness(uint instanceIndex, vec2 uv) {
    uint baseOffset = instanceIndex * uint(MATERIAL_STRIDE);
    int mrTexIdx = materials.data[baseOffset + 2u];

    float metallicFactor = intBitsToFloat(materials.data[baseOffset + 12u]);
    float roughnessFactor = intBitsToFloat(materials.data[baseOffset + 13u]);

    if (mrTexIdx >= 0) {
        vec4 mrTex = texture(sampler2D(textures[mrTexIdx], linearSampler), uv);
        // glTF spec: G=roughness, B=metallic
        return vec2(mrTex.b * metallicFactor, mrTex.g * roughnessFactor);
    }

    return vec2(metallicFactor, roughnessFactor);
}

// Get emissive color from material
vec3 getMaterialEmissive(uint instanceIndex, vec2 uv) {
    uint baseOffset = instanceIndex * uint(MATERIAL_STRIDE);
    int emissiveTexIdx = materials.data[baseOffset + 3u];

    float emissiveR = intBitsToFloat(materials.data[baseOffset + 14u]);
    float emissiveG = intBitsToFloat(materials.data[baseOffset + 15u]);
    float emissiveB = intBitsToFloat(materials.data[baseOffset + 16u]);
    vec3 emissiveFactor = vec3(emissiveR, emissiveG, emissiveB);

    if (emissiveTexIdx >= 0) {
        vec4 texColor = texture(sampler2D(textures[emissiveTexIdx], linearSampler), uv);
        return texColor.rgb * emissiveFactor;
    }

    return emissiveFactor;
}

// ============================================================================
// UV Interpolation
// ============================================================================

// Get interpolated UV from the UV buffer using barycentric coordinates
vec2 getHitUV(HitInfo hitInfo) {
    uint triIdx = hitInfo.triangleIndex;
    uint baseVertex = triIdx * 3u;

    vec2 uv0 = uvBuffer.uvs[baseVertex + 0u];
    vec2 uv1 = uvBuffer.uvs[baseVertex + 1u];
    vec2 uv2 = uvBuffer.uvs[baseVertex + 2u];

    float u = hitInfo.barycentricU;
    float v = hitInfo.barycentricV;
    float w = 1.0 - u - v;

    return w * uv0 + u * uv1 + v * uv2;
}

// ============================================================================
// Random Number Generation
// ============================================================================

// Persistent random number generator (evolves the seed)
float nextRandom(inout uint seed) {
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / 4294967296.0;
}

// ============================================================================
// Sampling Functions
// ============================================================================

// Sample cosine-weighted hemisphere
vec3 sampleCosineHemisphere(float r1, float r2) {
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt(r2);
    float sinTheta = sqrt(1.0 - r2);
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Sample GGX distribution for specular reflection
vec3 sampleGGX(float r1, float r2, float roughness) {
    float a = roughness * roughness;
    float a2 = max(a * a, 0.0001);

    float phi = 2.0 * PI * r1;
    float denom = max(1.0 + (a2 - 1.0) * r2, 0.0001);
    float cosTheta = sqrt((1.0 - r2) / denom);
    cosTheta = clamp(cosTheta, 0.0, 1.0);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));

    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// ============================================================================
// Tangent Space
// ============================================================================

// Build tangent frame from normal
void buildTangentFrame(vec3 N, out vec3 T, out vec3 B) {
    vec3 up = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// Transform vector from tangent space to world space
vec3 tangentToWorld(vec3 v, vec3 N, vec3 T, vec3 B) {
    return v.x * T + v.y * B + v.z * N;
}

// ============================================================================
// BRDF Functions
// ============================================================================

// Fresnel-Schlick approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// GGX/Trowbridge-Reitz normal distribution function
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0001);
}

// Smith's Schlick-GGX geometry function
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    float denom = NdotV * (1.0 - k) + k;

    return NdotV / max(denom, 0.0001);
}

// Smith's method for geometry obstruction
float geometrySmith(float NdotV, float NdotL, float roughness) {
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Cook-Torrance specular BRDF
vec3 cookTorranceSpecular(vec3 N, vec3 V, vec3 L, vec3 F0, float roughness) {
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Calculate terms
    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlick(HdotV, F0);

    // Cook-Torrance BRDF
    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL;

    return numerator / max(denominator, 0.0001);
}

// Lambert diffuse BRDF
vec3 lambertDiffuse(vec3 albedo) {
    return albedo / PI;
}

// ============================================================================
// Validation
// ============================================================================

// Check if a vector contains NaN or Inf
bool isValidVec3(vec3 v) {
    return !any(isnan(v)) && !any(isinf(v));
}

// Check if a float is valid
bool isValidFloat(float f) {
    return !isnan(f) && !isinf(f);
}

#endif // PBR_LIB_GLSL
