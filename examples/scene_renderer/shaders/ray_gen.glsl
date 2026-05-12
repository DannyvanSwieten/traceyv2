// Tier 0: minimal pinhole camera. Spawns one ray per pixel and stores its
// direction in payload.direction so future tiers can recover the view vector.

float hash(uint seed) {
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / 4294967296.0;
}

void shader(uvec2 pixelCoord, inout RayPayloads payloads) {
    uvec2 resolution = getResolution();

    // Seed must vary across dispatches, otherwise the same ray is retraced
    // and the running mean averages identical samples (no convergence).
    // The payload buffer is zero-initialised at the start of every
    // traceRays(), so payload.sampleIndex alone never differs across
    // dispatches — mix in `shaderInputs.currentSample` (CPU-incremented per
    // dispatch) to get a fresh jitter pattern per accumulation step.
    uint globalSampleIdx =
        uint(shaderInputs.currentSample - 1) * pc.samplesPerFrame +
        payloads.rayPayload.sampleIndex;
    payloads.rayPayload.rngSeed = pixelCoord.x + pixelCoord.y * resolution.x +
                                   globalSampleIdx * resolution.x * resolution.y;

    float jitterX = hash(payloads.rayPayload.rngSeed);
    float jitterY = hash(payloads.rayPayload.rngSeed + 1u);

    float width = float(resolution.x);
    float height = float(resolution.y);
    float aspectRatio = width / height;

    float tanHalfFov = tan(radians(shaderInputs.fov) / 2.0);
    float px = (2.0 * ((float(pixelCoord.x) + jitterX) / width) - 1.0) * tanHalfFov * aspectRatio;
    float py = (1.0 - 2.0 * ((float(pixelCoord.y) + jitterY) / height)) * tanHalfFov;

    vec3 origin = shaderInputs.cameraPosition;
    vec3 direction = normalize(
        shaderInputs.cameraForward +
        px * shaderInputs.cameraRight +
        py * shaderInputs.cameraUp
    );

    payloads.rayPayload.direction = direction;
    // payload.color is the running throughput along the path; the BSDF at each
    // hit multiplies it by the surface's reflectance, and sky_miss multiplies
    // it by the sky color to commit the final radiance.
    payloads.rayPayload.color = vec3(1.0);
    // Direct lighting accumulator. Hit shader adds light contributions
    // here; resolve sums `color + accum` so emissive surfaces and the sky
    // contribution keep working alongside explicit lights.
    payloads.rayPayload.accum = vec3(0.0);
    payloads.rayPayload.depth = 0u;
    payloads.rayPayload.alive = true;
    payloads.rayPayload.sampleIndex += 1u;

    traceRay(origin, 0.01, direction, 1000.0);
}
