#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// Fragment input from vertex shader
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in flat uint fragInstanceID;

// Fragment output
layout(location = 0) out vec4 outColor;

// Material buffer (same layout as path tracer)
layout(set = 0, binding = 0) readonly buffer MaterialBuffer {
    int data[];
} materials;

// Bindless textures
layout(set = 0, binding = 1) uniform sampler linearSampler;
layout(set = 0, binding = 2) uniform texture2D textures[];

// Camera/scene uniforms
layout(set = 0, binding = 3) uniform SceneUniforms {
    vec3 cameraPosition;
    vec3 lightDirection;    // Directional light direction (normalized)
    vec3 lightColor;        // Directional light color/intensity
    vec3 ambientLight;      // Ambient lighting
} scene;

// Constants
#define PI 3.14159265359
#define MATERIAL_STRIDE 20

// ============================================================================
// Material Access (same as path tracer pbr_lib.glsl)
// ============================================================================

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
// BRDF Functions (same as path tracer pbr_lib.glsl)
// ============================================================================

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0001);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    float denom = NdotV * (1.0 - k) + k;

    return NdotV / max(denom, 0.0001);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 cookTorranceSpecular(vec3 N, vec3 V, vec3 L, vec3 F0, float roughness) {
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlick(HdotV, F0);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL;

    return numerator / max(denominator, 0.0001);
}

vec3 lambertDiffuse(vec3 albedo) {
    return albedo / PI;
}

// ============================================================================
// Main Lighting
// ============================================================================

void main() {
    // Normalize interpolated normal
    vec3 N = normalize(fragNormal);

    // View direction (from fragment to camera)
    vec3 V = normalize(scene.cameraPosition - fragWorldPos);

    // Light direction (to light source)
    vec3 L = normalize(-scene.lightDirection);

    // Sample material properties
    vec3 albedo = getMaterialAlbedo(fragInstanceID, fragUV);
    vec2 metallicRoughness = getMaterialMetallicRoughness(fragInstanceID, fragUV);
    float metallic = metallicRoughness.x;
    float roughness = max(metallicRoughness.y, 0.04); // Clamp roughness to avoid artifacts
    vec3 emissive = getMaterialEmissive(fragInstanceID, fragUV);

    // Calculate F0 (reflectance at normal incidence)
    vec3 F0 = vec3(0.04); // Dielectric base reflectivity
    F0 = mix(F0, albedo, metallic);

    // Calculate lighting contribution
    float NdotL = max(dot(N, L), 0.0);

    // Compute PBR terms
    vec3 specular = cookTorranceSpecular(N, V, L, F0, roughness);
    vec3 diffuse = lambertDiffuse(albedo);

    // Fresnel for energy conservation
    vec3 F = fresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 kS = F;  // Specular contribution
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);  // Diffuse contribution

    // Combine diffuse and specular
    vec3 directLighting = (kD * diffuse + specular) * scene.lightColor * NdotL;

    // Add ambient lighting
    vec3 ambient = scene.ambientLight * albedo;

    // Add emissive
    vec3 color = directLighting + ambient + emissive;

    // Tone mapping (simple Reinhard)
    color = color / (color + vec3(1.0));

    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
