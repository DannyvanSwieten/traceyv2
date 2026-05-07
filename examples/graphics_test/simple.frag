#version 450

// Fragment input
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;

// Fragment output
layout(location = 0) out vec4 outColor;

void main() {
    // Simple shading: color based on normal (for testing)
    vec3 color = fragNormal * 0.5 + 0.5; // Map [-1,1] to [0,1]
    outColor = vec4(color, 1.0);
}
