#version 450

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec4 fragBaseColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 baseColor;
} pc;

void main() {
    fragBaseColor = pc.baseColor;
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    gl_PointSize = 8.0;
}
