#version 450

// PBR studio preview for the rasterizer viewport, driven by the engine's
// scene-level Light entities.
//
// The lights SSBO (set 0, binding 0) is shared with the path tracer's
// CompiledScene::lightBuffer. Each entry is six vec4s — see GPULight in
// src/scene/scene_compiler.hpp for the canonical layout; the LightRecord
// struct below mirrors it exactly.
//
// Per-light type:
//   • Point  (0) → direct lighting with 1/(d² + r²) attenuation
//   • Distant (1) → direct lighting, infinite distance ("sun")
//   • Dome   (2) → procedural sky/horizon/ground IBL gradient
//                  (first Dome wins; later Domes ignored — multi-IBL
//                  blending is a future feature)
//   • Area   (3) → approximated as a Distant pointing along the area's
//                  -Z. Path tracer integrates the rectangle properly;
//                  the viewport doesn't need pixel-perfect soft shadows.
//
// Fallback: with zero lights the shader emits unlit albedo so a fresh
// scene shows geometry rather than a black void.

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec4 fragAlbedo;
layout(location = 2) in vec3 fragEmissive;
layout(location = 3) in vec2 fragMR;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4  viewProj;
    vec4  viewPos;    // .xyz = camera world position
    uvec4 misc;       // .x = lightCount; .yzw reserved
} pc;

// std430-packed light records. Stride 96 bytes (6 × vec4) matches GPULight
// on the engine side. lights.data is unsized so the SSBO can hold any
// count; the loop bound is pc.misc.x (lightCount).
struct LightRecord {
    vec4 positionAndType;       // .xyz = world pos / area centre; .w = LightType
    vec4 directionAndIntensity; // .xyz = world dir (Distant / Area normal); .w = intensity
    vec4 colorAndExtraX;        // .xyz = colour; .w = Point.radius or Area.sizeX
    vec4 skyColorAndExtraY;     // .xyz = Dome sky;     .w = Area.sizeY
    vec4 horizonColorAndFlags;  // .xyz = Dome horizon; .w = reserved flags
    vec4 groundColorAndPad;     // .xyz = Dome ground;  .w = pad
};
layout(std430, set = 0, binding = 0) readonly buffer Lights {
    LightRecord data[];
} lights;

const float PI = 3.14159265359;

const int LIGHT_POINT   = 0;
const int LIGHT_DISTANT = 1;
const int LIGHT_DOME    = 2;
const int LIGHT_AREA    = 3;

// Sample a Dome's procedural sky/horizon/ground gradient along `dir`.
// dir.y == +1 → sky, dir.y == -1 → ground, dir.y ≈ 0 → horizon belt.
vec3 domeSample(LightRecord L, vec3 dir) {
    vec3 sky     = L.skyColorAndExtraY.rgb;
    vec3 horizon = L.horizonColorAndFlags.rgb;
    vec3 ground  = L.groundColorAndPad.rgb;
    float y = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 upper = mix(horizon, sky,    smoothstep(0.5, 1.0, y));
    vec3 lower = mix(horizon, ground, smoothstep(0.5, 0.0, y));
    return mix(lower, upper, smoothstep(0.45, 0.55, y));
}
vec3 domeSpecular(LightRecord L, vec3 N, vec3 R, float roughness) {
    // Roughness lerps between mirror reflection and matte-hemisphere
    // average. Cheap stand-in for a prefiltered cubemap — accurate
    // enough for the viewport's "is the light coming from up there?"
    // judgement.
    return mix(domeSample(L, R), domeSample(L, N), roughness);
}

// Face-normal reconstruction from world-position derivatives. The
// abs/sign trick aligns the normal with the view direction so back-face
// shading reads consistently regardless of winding order.
vec3 faceNormal(vec3 worldPos, vec3 V) {
    vec3 dx = dFdx(worldPos);
    vec3 dy = dFdy(worldPos);
    vec3 N  = normalize(cross(dy, dx));
    if (dot(N, V) < 0.0) N = -N;
    return N;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    vec3 Fr = max(vec3(1.0 - roughness), F0) - F0;
    return F0 + Fr * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
float distributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}
float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness) *
           geometrySchlickGGX(NdotL, roughness);
}

vec3 directLight(vec3 N, vec3 V, vec3 L, vec3 lightColor,
                 vec3 albedo, float metallic, float roughness, vec3 F0)
{
    vec3 H = normalize(L + V);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3  F = fresnelSchlick(VdotH, F0);

    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;
    return (diffuse + specular) * lightColor * NdotL;
}

void main() {
    vec3  albedo    = clamp(fragAlbedo.rgb, vec3(0.0), vec3(1.0));
    float metallic  = clamp(fragMR.x, 0.0, 1.0);
    float roughness = clamp(fragMR.y, 0.04, 1.0);
    vec3 V = normalize(pc.viewPos.xyz - fragWorldPos);
    vec3 N = faceNormal(fragWorldPos, V);
    vec3 R = reflect(-V, N);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NdotV = max(dot(N, V), 0.0);

    vec3 directAccum = vec3(0.0);
    vec3 iblAccum    = vec3(0.0);

    // First-Dome-wins: pick up the env-light shape from the first Dome
    // we encounter, then add direct contributions from every other type.
    bool haveDome = false;
    LightRecord domeLight;

    uint count = pc.misc.x;
    for (uint i = 0u; i < count; ++i) {
        LightRecord Li = lights.data[i];
        int type = int(Li.positionAndType.w);
        vec3  col = Li.colorAndExtraX.rgb * Li.directionAndIntensity.w;

        if (type == LIGHT_DOME) {
            if (!haveDome) {
                domeLight = Li;
                haveDome  = true;
            }
            continue;
        }

        // Direction-of-incidence (toward the surface, normalised). For
        // Distant lights this is -(world direction of the light's
        // local -Z). For Point/Area we use the vector to the centre.
        vec3 L;
        float falloff = 1.0;
        if (type == LIGHT_DISTANT) {
            L = normalize(-Li.directionAndIntensity.xyz);
        } else if (type == LIGHT_AREA) {
            // Rasterizer approximation: treat the area light as a Distant
            // along its -Z. Path tracer integrates the rectangle for soft
            // shadows; the viewport stays cheap and stable. Intensity
            // scales by surface area so the artist's "bigger panel =
            // brighter" intuition holds.
            float w = Li.colorAndExtraX.w;
            float h = Li.skyColorAndExtraY.w;
            falloff = max(w * h, 1e-4);
            L = normalize(-Li.directionAndIntensity.xyz);
        } else {
            // Point: vector from fragment to light, 1/(d² + r²) falloff.
            vec3 toLight = Li.positionAndType.xyz - fragWorldPos;
            float d2 = max(dot(toLight, toLight), 1e-6);
            float r  = Li.colorAndExtraX.w;  // soft radius
            falloff  = 1.0 / (d2 + r * r);
            L = toLight * inversesqrt(d2);
        }

        directAccum += directLight(N, V, L, col * falloff,
                                   albedo, metallic, roughness, F0);
    }

    // Dome contributes IBL diffuse + specular when present. If no Dome
    // exists, the lit surface only receives direct-light contributions —
    // we explicitly DON'T fall back to a hardcoded sky so an authored
    // "scene with no Dome" matches between rasterizer and path tracer.
    if (haveDome) {
        vec3 domeTint  = domeLight.colorAndExtraX.rgb
                        * domeLight.directionAndIntensity.w;
        vec3 F_ibl     = fresnelSchlickRoughness(NdotV, F0, roughness);
        vec3 kD_ibl    = (vec3(1.0) - F_ibl) * (1.0 - metallic);
        vec3 diffIBL   = domeSample(domeLight, N) * albedo;
        vec3 specIBL   = domeSpecular(domeLight, N, R, roughness) * F_ibl;
        iblAccum       = (kD_ibl * diffIBL + specIBL) * domeTint;
    }

    vec3 color = directAccum + iblAccum + fragEmissive;

    // No scene lights yet — e.g. modeling an asset before it's lit. Flat unlit
    // albedo hides all form (you can't even see faces), so light it with a camera
    // "headlight" plus a little ambient so the shape reads. Viewport aid only: as
    // soon as the scene has real lights they take over (this branch isn't reached
    // when lightCount > 0), so it never fights authored lighting.
    if (pc.misc.x == 0u) {
        vec3 headDir = normalize(V + vec3(0.0, 0.35, 0.0)); // key from just above the camera
        vec3 lit     = directLight(N, V, headDir, vec3(3.0),
                                   albedo, metallic, roughness, F0);
        color = lit + albedo * 0.20 + fragEmissive;          // + ambient to lift the shadow side
    }

    // Last-ditch NaN guard: any numerical hiccup falls back to flat
    // albedo + a sky tint so the surface is never invisible.
    if (any(isnan(color)) || any(isinf(color))) {
        color = albedo * 0.6 + vec3(0.1, 0.12, 0.15);
    }

    color = max(color, vec3(0.0));
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, clamp(fragAlbedo.a, 0.0, 1.0));
}
