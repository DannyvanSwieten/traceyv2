// PBR Library for ISF shaders
// Common functions and utilities for physically-based rendering

#ifndef PBR_LIB_GLSL
#define PBR_LIB_GLSL

#define PI 3.14159265359

// ============================================================================
// Material Data Access
// ============================================================================

// GPUMaterial layout (96 bytes = 24 ints):
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
// offset 17: clearcoat (float)
// offset 18: clearcoatRoughness (float)
// offset 19: padding
// offset 20: sheenColorR (float)
// offset 21: sheenColorG (float)
// offset 22: sheenColorB (float)
// offset 23: sheenRoughness (float)
#define MATERIAL_STRIDE 24

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

// Get normal from material (returns tangent-space normal)
vec3 getMaterialNormal(uint instanceIndex, vec2 uv) {
    uint baseOffset = instanceIndex * uint(MATERIAL_STRIDE);
    int normalTexIdx = materials.data[baseOffset + 1u];

    if (normalTexIdx >= 0) {
        vec3 normalSample = texture(sampler2D(textures[normalTexIdx], linearSampler), uv).rgb;
        // Convert from [0,1] to [-1,1] tangent space
        return normalize(normalSample * 2.0 - 1.0);
    }

    // No normal map, return default tangent-space up vector
    return vec3(0.0, 0.0, 1.0);
}

// Get clearcoat parameters from material
vec2 getMaterialClearcoat(uint instanceIndex) {
    uint baseOffset = instanceIndex * uint(MATERIAL_STRIDE);
    float clearcoat = intBitsToFloat(materials.data[baseOffset + 17u]);
    float clearcoatRoughness = intBitsToFloat(materials.data[baseOffset + 18u]);
    return vec2(clearcoat, clearcoatRoughness);
}

// Get sheen parameters from material
// Returns: (sheenColor.r, sheenColor.g, sheenColor.b, sheenRoughness) as vec4
vec4 getMaterialSheen(uint instanceIndex) {
    uint baseOffset = instanceIndex * uint(MATERIAL_STRIDE);
    float sheenR = intBitsToFloat(materials.data[baseOffset + 20u]);
    float sheenG = intBitsToFloat(materials.data[baseOffset + 21u]);
    float sheenB = intBitsToFloat(materials.data[baseOffset + 22u]);
    float sheenRoughness = intBitsToFloat(materials.data[baseOffset + 23u]);
    return vec4(sheenR, sheenG, sheenB, sheenRoughness);
}

// ============================================================================
// UV Interpolation
// ============================================================================

// Get interpolated UV from the UV buffer using barycentric coordinates
vec2 getHitUV(HitInfo hitInfo) {
    // Get vertex offset for this instance's BLAS
    uint vertexOffset = vertexOffsets.offsets[hitInfo.instanceIndex];

    // Add offset to local triangle index to get global buffer index
    uint triIdx = hitInfo.triangleIndex;
    uint baseVertex = vertexOffset + triIdx * 3u;

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
// Clearcoat Functions
// ============================================================================

// Clearcoat uses a fixed IOR of 1.5 (F0 = 0.04) for the top layer
#define CLEARCOAT_F0 vec3(0.04)
#define CLEARCOAT_IOR 1.5

// Fresnel for clearcoat (uses Schlick approximation)
vec3 fresnelSchlickClearcoat(float cosTheta) {
    return CLEARCOAT_F0 + (1.0 - CLEARCOAT_F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// Sample clearcoat direction using GGX
// Returns the sampled direction in world space
vec3 sampleClearcoat(float r1, float r2, float clearcoatRoughness, vec3 N, vec3 V, vec3 T, vec3 B, out vec3 weight) {
    // Sample GGX microfacet normal in tangent space
    vec3 H_local = sampleGGX(r1, r2, clearcoatRoughness);
    vec3 H = normalize(tangentToWorld(H_local, N, T, B));

    // Reflect view direction around microfacet normal
    vec3 L = reflect(-V, H);

    float NdotL = max(dot(N, L), 0.001);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.001);
    float VdotH = max(dot(V, H), 0.001);

    // Clearcoat BRDF with fixed F0 = 0.04
    vec3 F = fresnelSchlickClearcoat(VdotH);
    float G = geometrySmith(NdotV, NdotL, clearcoatRoughness);

    // Importance sampling weight for GGX
    weight = F * G * VdotH / (NdotV * NdotH);

    return L;
}

// Evaluate clearcoat BRDF for a given direction
vec3 evaluateClearcoat(vec3 N, vec3 V, vec3 L, float clearcoatRoughness) {
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = distributionGGX(NdotH, clearcoatRoughness);
    float G = geometrySmith(NdotV, NdotL, clearcoatRoughness);
    vec3 F = fresnelSchlickClearcoat(VdotH);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL;

    return numerator / max(denominator, 0.0001);
}

// Attenuation of base layer due to clearcoat Fresnel reflection
// Returns 1.0 - clearcoat_fresnel to scale base layer contribution
float clearcoatAttenuation(float VdotN) {
    float F = 0.04 + (1.0 - 0.04) * pow(1.0 - VdotN, 5.0);
    return 1.0 - F;
}

// ============================================================================
// Sheen Functions (for fabric/cloth materials)
// ============================================================================

// Charlie distribution for sheen (Ashikhmin's velvet/cloth BRDF)
// This is a softer falloff than GGX, suitable for fabric
float distributionCharlie(float NdotH, float sheenRoughness) {
    float a = max(sheenRoughness * sheenRoughness, 0.0001);
    float invA = 1.0 / a;
    float cos2H = NdotH * NdotH;
    float sin2H = max(1.0 - cos2H, 0.0);

    // Charlie distribution: D = (2 + 1/a) * sin^(1/a) / (2*pi)
    float D = (2.0 + invA) * pow(sin2H, invA * 0.5) / (2.0 * PI);
    return D;
}

// Ashikhmin visibility term for sheen
float visibilityAshikhmin(float NdotV, float NdotL) {
    return 1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV));
}

// Sheen Fresnel - uses Schlick with sheen color as F0
vec3 fresnelSchlickSheen(float cosTheta, vec3 sheenColor) {
    return sheenColor + (1.0 - sheenColor) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// Evaluate sheen BRDF
vec3 evaluateSheen(vec3 N, vec3 V, vec3 L, vec3 sheenColor, float sheenRoughness) {
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = distributionCharlie(NdotH, sheenRoughness);
    float Vis = visibilityAshikhmin(NdotV, NdotL);
    vec3 F = fresnelSchlickSheen(VdotH, sheenColor);

    return D * Vis * F;
}

// Sample sheen direction using cosine-weighted hemisphere
// Sheen uses diffuse-like sampling since Charlie doesn't have efficient importance sampling
vec3 sampleSheen(float r1, float r2, vec3 N, vec3 T, vec3 B, vec3 sheenColor, float sheenRoughness, out vec3 weight) {
    // Use cosine-weighted sampling for sheen
    vec3 L_local = sampleCosineHemisphere(r1, r2);
    vec3 L = normalize(tangentToWorld(L_local, N, T, B));

    float NdotL = max(dot(N, L), 0.001);

    // Weight = BRDF * cos(theta) / PDF = sheen * cos / (cos/pi) = sheen * pi
    vec3 sheenBrdf = evaluateSheen(N, normalize(reflect(-L, N)), L, sheenColor, sheenRoughness);
    weight = sheenBrdf * PI;
    weight = clamp(weight, vec3(0.0), vec3(5.0)); // Prevent fireflies

    return L;
}

// ============================================================================
// Combined PBR with Clearcoat and Sheen
// ============================================================================

// Full PBR material structure
struct PBRMaterial {
    vec3 albedo;
    float metallic;
    float roughness;
    vec3 emission;

    // Clearcoat layer
    float clearcoat;           // Clearcoat strength [0, 1]
    float clearcoatRoughness;  // Clearcoat roughness

    // Sheen layer
    vec3 sheenColor;           // Sheen color (often white or fabric tint)
    float sheenRoughness;      // Sheen roughness
};

// Lobe selection enum
#define LOBE_DIFFUSE 0
#define LOBE_SPECULAR 1
#define LOBE_CLEARCOAT 2
#define LOBE_SHEEN 3

// Calculate lobe probabilities based on material properties
// Returns vec4: (diffuse, specular, clearcoat, sheen) probabilities
vec4 getLobeProbabilities(PBRMaterial mat) {
    float sheenWeight = length(mat.sheenColor);

    // Base diffuse probability (reduced by metallic and clearcoat)
    float pDiffuse = (1.0 - mat.metallic) * (1.0 - mat.clearcoat * 0.5);

    // Specular probability (metals are fully specular, dielectrics have some)
    float pSpecular = mix(0.04, 1.0, mat.metallic);

    // Clearcoat probability
    float pClearcoat = mat.clearcoat;

    // Sheen probability (only if sheen color is present)
    float pSheen = sheenWeight > 0.01 ? 0.3 * sheenWeight : 0.0;

    // Normalize probabilities
    float total = pDiffuse + pSpecular + pClearcoat + pSheen;
    if (total < 0.0001) total = 1.0;

    return vec4(pDiffuse, pSpecular, pClearcoat, pSheen) / total;
}

// Select a lobe to sample based on random number and probabilities
int selectLobe(float r, vec4 probs) {
    float cdf = probs.x; // diffuse
    if (r < cdf) return LOBE_DIFFUSE;

    cdf += probs.y; // specular
    if (r < cdf) return LOBE_SPECULAR;

    cdf += probs.z; // clearcoat
    if (r < cdf) return LOBE_CLEARCOAT;

    return LOBE_SHEEN;
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
