// Tier 3: terminating radiance source for the path. Commits the accumulated
// throughput by multiplying it with the environment radiance sampled along
// the current ray direction.
//
// If the scene contains a Dome light, sample its procedural sky/horizon/
// ground gradient (matching what the rasterizer renders in the viewport).
// Otherwise fall back to the original hard-coded sunset gradient so
// projects without an authored Dome still render against a recognisable
// background instead of pure black.
//
// Stride 6 vec4 / light matches GPULight in scene_compiler.hpp.

vec3 sampleDomeGradient(uint domeIdx, vec3 dir) {
    vec3 sky     = lights.data[domeIdx * 6u + 3u].xyz;  // skyColor
    vec3 horizon = lights.data[domeIdx * 6u + 4u].xyz;  // horizonColor
    vec3 ground  = lights.data[domeIdx * 6u + 5u].xyz;  // groundColor
    float y = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 upper = mix(horizon, sky,    smoothstep(0.5, 1.0, y));
    vec3 lower = mix(horizon, ground, smoothstep(0.5, 0.0, y));
    return mix(lower, upper, smoothstep(0.45, 0.55, y));
}

void shader(inout RayPayloads payloads) {
    if (!payloads.rayPayload.alive) return;

    vec3 throughput = payloads.rayPayload.color;
    vec3 dir = payloads.rayPayload.direction;

    vec3 sky;
    // First-Dome-wins: same convention as the rasterizer fragment shader.
    bool haveDome = false;
    uint domeIdx = 0u;
    for (uint li = 0u; li < shaderInputs.lightCount; ++li) {
        if (int(lights.data[li * 6u + 0u].w) == 2) {  // LightType::Dome
            domeIdx  = li;
            haveDome = true;
            break;
        }
    }
    if (haveDome) {
        vec3 tint = lights.data[domeIdx * 6u + 2u].xyz
                  * lights.data[domeIdx * 6u + 1u].w;  // colour * intensity
        sky = sampleDomeGradient(domeIdx, dir) * tint;
    } else {
        // Fallback gradient: warm horizon, deep blue zenith. Picks a
        // saturated palette so even after Reinhard tonemap the gradient
        // stays recognisably sky-coloured.
        float t = clamp(0.5 * (dir.y + 1.0), 0.0, 1.0);
        vec3 horizon = vec3(1.0, 0.55, 0.20);
        vec3 zenith  = vec3(0.15, 0.35, 1.00);
        sky = mix(horizon, zenith, t);
    }

    payloads.rayPayload.color = throughput * sky;
    payloads.rayPayload.alive = false;
}
