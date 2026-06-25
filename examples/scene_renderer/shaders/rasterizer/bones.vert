#version 450

// Skeleton overlay: a world-space LINE_LIST of bone segments fed from a vertex
// buffer (one Vec3 per endpoint, two per bone). Transformed by the camera MVP
// and drawn depth-test OFF so the skeleton reads on top of the skinned mesh.
// Flat color comes from the shared push-constant (baseColor.rgb); reuses the
// gizmo fragment shader.

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 vColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 baseColor; // rgb = bone line color
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vColor = pc.baseColor.rgb;
}
