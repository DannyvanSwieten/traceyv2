#version 450

// Vertex input - position only
layout(location = 0) in vec3 inPosition;

// Vertex output - world position for normal calculation and base color
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec4 fragBaseColor;

// Push constants for MVP matrix and material base color
layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 baseColor;
} pc;

void main() {
    // Pass world position and base color to fragment shader
    fragWorldPos = inPosition;
    fragBaseColor = pc.baseColor;
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
}
