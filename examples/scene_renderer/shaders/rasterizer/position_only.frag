#version 450

// Input from vertex shader
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec4 fragBaseColor;

// Output color
layout(location = 0) out vec4 outColor;

void main() {
    // Calculate flat normals using screen-space derivatives
    vec3 dFdxPos = dFdx(fragWorldPos);
    vec3 dFdyPos = dFdy(fragWorldPos);
    vec3 normal = normalize(cross(dFdxPos, dFdyPos));

    // Simple directional lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 viewDir = vec3(0.0, 0.0, 1.0); // Simple view direction

    // Ambient
    float ambient = 0.3;

    // Diffuse
    float diffuse = max(dot(normal, lightDir), 0.0) * 0.6;

    // Specular (simple Blinn-Phong)
    vec3 halfDir = normalize(lightDir + viewDir);
    float specular = pow(max(dot(normal, halfDir), 0.0), 32.0) * 0.2;

    // Use material base color from push constants
    vec3 baseColor = fragBaseColor.rgb;

    // Combine lighting
    vec3 finalColor = baseColor * (ambient + diffuse) + vec3(specular);

    outColor = vec4(finalColor, fragBaseColor.a);
}
