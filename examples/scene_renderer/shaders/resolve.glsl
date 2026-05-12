// Tier 2: Welford running mean into the linear-HDR accumulator, then
// Reinhard tonemap + sRGB gamma into the display-ready output image.

void shader(uvec2 pixel, in RayPayloads payloads) {
    // `color` is the throughput committed at miss / emission; `accum` is the
    // direct-lighting contribution gathered during each hit. Their sum is
    // the radiance estimate for this sample.
    vec3 sampleColor = payloads.rayPayload.color + payloads.rayPayload.accum;

    // 1-based sample number being folded into the running mean.
    //   currentSample      — 1-based count of render() calls since clear
    //   pc.samplesPerFrame — samples per traceRays() batch
    //   pc.sampleIndex     — 0-based position within the current batch
    int n = (shaderInputs.currentSample - 1) * int(pc.samplesPerFrame)
          + int(pc.sampleIndex) + 1;

    vec4 prev = imageLoad(accumulatorImage, ivec2(pixel));
    vec3 avg = prev.rgb + (sampleColor - prev.rgb) / float(n);
    imageStore(accumulatorImage, ivec2(pixel), vec4(avg, 1.0));

    vec3 tonemapped = avg / (avg + vec3(1.0));
    vec3 gammaCorrected = pow(tonemapped, vec3(1.0 / 2.2));
    imageStore(outputImage, ivec2(pixel), vec4(gammaCorrected, 1.0));
}
