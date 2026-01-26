#version 450

// Vertex input (fixed layout)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// Vertex output to fragment shader
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out flat uint fragInstanceID;

// Push constants for MVP matrices
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 view;
    mat4 proj;
    uint instanceID;  // For material lookup
} pc;

void main() {
    // Transform vertex position to world space
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;

    // Transform normal to world space (assuming uniform scaling)
    // For non-uniform scaling, use transpose(inverse(model))
    mat3 normalMatrix = mat3(pc.model);
    fragNormal = normalize(normalMatrix * inNormal);

    // Pass through UV coordinates
    fragUV = inUV;

    // Pass instance ID for material lookup
    fragInstanceID = pc.instanceID;

    // Final position in clip space
    gl_Position = pc.proj * pc.view * worldPos;
}
