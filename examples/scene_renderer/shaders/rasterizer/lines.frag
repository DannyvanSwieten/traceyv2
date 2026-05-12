#version 450

layout(location = 0) in vec4 fragBaseColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Wireframe: dim the base color slightly so edges read against the fill.
    // No lighting because dFdx/dFdy are undefined for line rasterization.
    outColor = vec4(fragBaseColor.rgb * 0.85, 1.0);
}
