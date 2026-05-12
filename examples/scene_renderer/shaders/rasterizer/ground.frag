#version 450

// Reference-grid shader on the y=0 plane. Two scales of grid lines (minor and
// major), thicker world-axis highlights, and a distance fade from the camera
// position so the grid doesn't shimmer at the horizon. Uses fwidth for
// derivative-based line anti-aliasing.

layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 baseColor; // baseColor.xyz = camera world position
} pc;

float gridFactor(vec2 p, float scale, float lineWidth) {
    vec2 coord = p / scale;
    vec2 derivative = fwidth(coord);
    // Distance from the nearest integer line in pixels (after division by
    // fwidth). min() picks whichever of the two perpendicular lines we are
    // closer to.
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / max(derivative, vec2(1e-5));
    float line = min(grid.x, grid.y);
    return 1.0 - smoothstep(0.0, lineWidth, line);
}

float axisLine(float coord, float lineWidth) {
    float w = max(fwidth(coord), 1e-5);
    return 1.0 - smoothstep(0.0, lineWidth * w, abs(coord));
}

void main() {
    vec2 p = vWorldPos.xz;

    float minor = gridFactor(p,  1.0, 1.0);
    float major = gridFactor(p, 10.0, 1.5);

    // World X-axis runs along z = 0 (highlight red).
    // World Z-axis runs along x = 0 (highlight blue).
    float axisX = axisLine(vWorldPos.z, 1.5);
    float axisZ = axisLine(vWorldPos.x, 1.5);

    vec3 minorColor = vec3(0.45);
    vec3 majorColor = vec3(0.65);
    vec3 axisRed    = vec3(0.75, 0.30, 0.30);
    vec3 axisBlue   = vec3(0.30, 0.45, 0.80);

    vec3 color = mix(minorColor, majorColor, major);
    float alpha = max(minor * 0.55, major * 0.85);

    if (axisX > 0.0) {
        color = mix(color, axisRed,  axisX);
        alpha = max(alpha, axisX);
    }
    if (axisZ > 0.0) {
        color = mix(color, axisBlue, axisZ);
        alpha = max(alpha, axisZ);
    }

    // Distance fade so the grid disappears toward the horizon. Measured from
    // the camera position so the fade is roughly view-space-symmetric.
    vec2 cam = pc.baseColor.xz;
    float dist = length(p - cam);
    float fade = 1.0 - smoothstep(40.0, 200.0, dist);
    alpha *= fade;

    if (alpha < 0.001) discard;

    outColor = vec4(color, alpha);
}
