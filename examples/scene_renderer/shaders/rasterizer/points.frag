#version 450

layout(location = 0) in vec4 fragBaseColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Map gl_PointCoord [0,1] to centered [-1,1]; discard outside circle.
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;

    // Smoothstep edge fade for antialiased boundary (8px points → ~1px feather).
    float alpha = 1.0 - smoothstep(0.85, 1.0, r2);
    outColor = vec4(fragBaseColor.rgb, fragBaseColor.a * alpha);
}
