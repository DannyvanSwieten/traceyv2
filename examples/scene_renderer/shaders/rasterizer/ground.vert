#version 450

// Procedurally emits the four corners of a large quad lying on the y=0 plane.
// No vertex buffer required — the index alone drives the position. Drawn as a
// triangle strip from gl_VertexIndex 0..3.

layout(location = 0) out vec3 vWorldPos;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    // baseColor.xyz carries the camera world position so the fragment shader
    // can fade the grid based on viewer distance; baseColor.w is unused.
    vec4 baseColor;
} pc;

const float GROUND_HALF = 500.0;

void main() {
    vec2 corners[4] = vec2[4](
        vec2(-GROUND_HALF, -GROUND_HALF),
        vec2( GROUND_HALF, -GROUND_HALF),
        vec2(-GROUND_HALF,  GROUND_HALF),
        vec2( GROUND_HALF,  GROUND_HALF)
    );
    vec2 xz = corners[gl_VertexIndex];
    vWorldPos = vec3(xz.x, 0.0, xz.y);
    gl_Position = pc.mvp * vec4(vWorldPos, 1.0);
}
