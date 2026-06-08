#version 450

layout(location = 0) in vec3 inPosition;
// Per-vertex Cd. The SceneCompiler always allocates this buffer (defaults
// to white when the geometry doesn't carry Cd) so the binding is safe;
// when an attribute_vop writes Cd, the colour shows up here. Multiplied
// with baseColor below so a material-tinted instance still picks up the
// VOP-authored variation per point.
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 fragBaseColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 baseColor;
} pc;

void main() {
    fragBaseColor = vec4(pc.baseColor.rgb * inColor, pc.baseColor.a);
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    gl_PointSize = 8.0;
}
