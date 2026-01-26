#version 450

// Vertex input
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// Vertex output
layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;

// Uniforms (simple MVP matrix)
layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragNormal = inNormal;
    fragUV = inUV;
}
