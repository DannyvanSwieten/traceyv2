#version 450

// Per-vertex inputs (binding 0 = position, binding 1 = Cd). SceneCompiler
// always allocates a white-fill Cd buffer when the SceneObject has no
// vertex colours, so the second binding is always defined.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

// Per-instance inputs (binding 2, INPUT_RATE_INSTANCE). Layout matches
// vulkan_graphics_pipeline.cpp's `bindingDescriptions[2]` and the
// rasterizer's InstanceData struct: model + albedo + (metallic,
// roughness, emission strength) + emission colour.
layout(location = 2) in vec4 inModel0;
layout(location = 3) in vec4 inModel1;
layout(location = 4) in vec4 inModel2;
layout(location = 5) in vec4 inModel3;
layout(location = 6) in vec4 inAlbedo;     // rgb = albedo, a = opacity
layout(location = 7) in vec4 inMRX;        // x=metallic, y=roughness, z=emissive strength
layout(location = 8) in vec4 inEmissive;   // rgb = emissive colour

// Outputs to the fragment stage. We carry world-space position so the
// frag's screen-space derivative can compute a faceted normal that's
// stable in world space (matches the previous behaviour). The PBR
// fragment shader reads albedo/metallic/roughness/emissive from these
// interpolants — they're flat-shaded per instance because every vertex
// of an instance has identical interpolation values, so the GPU
// effectively constant-folds them.
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec4 fragAlbedo;
layout(location = 2) out vec3 fragEmissive;
layout(location = 3) out vec2 fragMR;      // x=metallic, y=roughness

layout(push_constant) uniform PushConstants {
    mat4  viewProj;
    vec4  viewPos;   // xyz = camera world position; w unused.
    uvec4 misc;      // .x = lightCount (fragment-only); .yzw reserved
} pc;

void main() {
    mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);
    vec4 worldPos = model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;

    // Compose per-instance albedo with per-vertex Cd. Default Cd is
    // white, so a regular instance picks up its instance tint
    // unchanged; a VOP-driven Cd modulates it.
    fragAlbedo = vec4(inAlbedo.rgb * inColor, inAlbedo.a);
    fragEmissive = inEmissive.rgb * inMRX.z;
    fragMR = vec2(inMRX.x, inMRX.y);

    gl_Position = pc.viewProj * worldPos;
}
