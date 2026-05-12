#version 450

// Vertex input
//   binding 0 = position (vec3), always present.
//   binding 1 = per-vertex Cd (vec3), always present — the SceneCompiler
//               allocates a white-fill buffer when the SceneObject has no
//               vertex colors, so this attribute is always defined.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

// Vertex output - world position (for screen-derivative flat normals) and
// composed base color (push-constant baseColor × per-vertex Cd).
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec4 fragBaseColor;

// Push constants for MVP matrix and material base color
layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 baseColor;
} pc;

void main() {
    fragWorldPos = inPosition;
    // Multiply the material's base color by the per-vertex Cd. Default Cd
    // is white, so objects without vertex colors render exactly as before.
    fragBaseColor = vec4(pc.baseColor.rgb * inColor, pc.baseColor.a);
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
}
