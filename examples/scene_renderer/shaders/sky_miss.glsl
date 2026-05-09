// Tier 3: terminating radiance source for the path. Commits the accumulated
// throughput by multiplying it with a horizon-to-zenith gradient sampled
// along the current ray direction.

void shader(inout RayPayloads payloads) {
    if (!payloads.rayPayload.alive) return;

    vec3 throughput = payloads.rayPayload.color;
    float t = clamp(0.5 * (payloads.rayPayload.direction.y + 1.0), 0.0, 1.0);
    // Saturated sunset-style gradient. Picks a warm horizon and a deep
    // blue zenith so even after Reinhard tonemap the gradient stays
    // recognisably sky-coloured.
    vec3 horizon = vec3(1.0, 0.55, 0.20);
    vec3 zenith  = vec3(0.15, 0.35, 1.00);
    vec3 sky = mix(horizon, zenith, t);

    payloads.rayPayload.color = throughput * sky;
    payloads.rayPayload.alive = false;
}
