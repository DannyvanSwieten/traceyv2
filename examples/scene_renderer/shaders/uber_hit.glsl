// Tier 5: full PBR BSDF, but every per-shading-point attribute (albedo,
// metallic, roughness, emission, normal) is sourced from the material VM.
// The host fetches raw values from the materials SSBO and feeds them as
// MatInputs; the VM program decides how to transform them and what to
// emit. The default passthrough graph yields exactly the tier-4 output,
// so this is a pure plumbing change at parity.

#include "pbr_lib.glsl"
#include "material_vm.glsl"

void shader(HitInfo hitInfo, inout RayPayloads payloads) {
    if (!payloads.rayPayload.alive) return;

    if (payloads.rayPayload.depth >= shaderInputs.maxDepth) {
        payloads.rayPayload.color = vec3(0.0);
        payloads.rayPayload.alive = false;
        return;
    }

    vec3 N = normalize(vec3(hitInfo.normalX, hitInfo.normalY, hitInfo.normalZ));
    vec3 incomingDir = normalize(payloads.rayPayload.direction);
    vec3 V = -incomingDir;
    if (dot(N, V) < 0.0) N = -N;

    vec3 hitPos = getWorldHitPosition(g_CurrentRayIndex, hitInfo);
    vec2 uv = getHitUV(hitInfo);

    vec3 hostAlbedo = getMaterialAlbedo(hitInfo.instanceIndex, uv);
    vec3 hostEmission = getMaterialEmissive(hitInfo.instanceIndex, uv);
    vec2 hostMR = getMaterialMetallicRoughness(hitInfo.instanceIndex, uv);

    vec3 T, B;
    buildTangentFrame(N, T, B);

    MatInputs vmIn;
    vmIn.albedo = hostAlbedo;
    vmIn.metallic = hostMR.x;
    vmIn.roughness = hostMR.y;
    vmIn.emission = hostEmission;
    vmIn.normal = vec3(0.0, 0.0, 1.0);
    vmIn.viewDir = V;
    vmIn.worldPosition = hitPos;
    vmIn.worldNormal = N;
    vmIn.worldTangent = T;
    vmIn.uv0 = uv;
    vmIn.uv1 = uv;

    uint programId = instanceProgramIndex.indices[hitInfo.instanceIndex];
    MatResult mat = runMaterialProgram(programId, vmIn);

    vec3 albedo = mat.albedo;
    vec3 emission = mat.emission;
    float metallic = mat.metallic;
    float roughness = clamp(mat.roughness, 0.04, 1.0);

    if (length(emission) > 0.0) {
        payloads.rayPayload.color *= emission;
        payloads.rayPayload.alive = false;
        return;
    }

    // Next-event estimation: iterate all scene lights and accumulate their
    // direct (unshadowed) contribution into the payload's accum slot. We
    // weight by the running throughput so deeper bounces still receive
    // light — the BRDF multiplier for the *current* surface is folded in
    // here via the Lambert diffuse term (specular highlights still arrive
    // through BSDF importance sampling at the bounce trace). Shadow rays
    // are not yet supported by the wavefront pipeline (traceRay only
    // schedules into the next-bounce queue), so this is "lights ignore
    // occlusion" for v1 — visually wrong through walls, but still
    // illuminates scenes correctly when geometry doesn't intervene.
    if (shaderInputs.lightCount > 0u) {
        vec3 diffuseBrdf = albedo * (1.0 - metallic) * (1.0 / 3.14159265);
        for (uint li = 0u; li < shaderInputs.lightCount; ++li) {
            vec4 posType   = lights.data[li * 3u + 0u];
            vec4 dirIntens = lights.data[li * 3u + 1u];
            vec4 colorPad  = lights.data[li * 3u + 2u];

            vec3 Ldir;
            float falloff;
            int ltype = int(posType.w);
            if (ltype == 0) {
                // Point: attenuate by inverse-square distance.
                vec3 toLight = posType.xyz - hitPos;
                float distSq = max(dot(toLight, toLight), 1e-4);
                Ldir = toLight * inversesqrt(distSq);
                falloff = 1.0 / distSq;
            } else {
                // Distant: light is at infinity; direction stored in slot
                // 1.xyz already points from light *toward* the scene
                // (actor world-forward), so flip it to get L (surface → light).
                Ldir = -normalize(dirIntens.xyz);
                falloff = 1.0;
            }

            float NdotLlight = max(dot(N, Ldir), 0.0);
            if (NdotLlight <= 0.0) continue;

            vec3 Li = colorPad.xyz * dirIntens.w * falloff;
            payloads.rayPayload.accum +=
                payloads.rayPayload.color * diffuseBrdf * Li * NdotLlight;
        }
    }

    float r1 = nextRandom(payloads.rayPayload.rngSeed);
    float r2 = nextRandom(payloads.rayPayload.rngSeed);
    float r3 = nextRandom(payloads.rayPayload.rngSeed);

    vec3 L;
    vec3 throughput;
    float NdotV = max(dot(N, V), 0.001);

    if (r3 < metallic) {
        vec3 H_local = sampleGGX(r1, r2, roughness);
        vec3 H = normalize(tangentToWorld(H_local, N, T, B));
        L = reflect(-V, H);

        float NdotL = dot(L, N);
        if (NdotL <= 0.0) {
            L = reflect(-V, N);
            NdotL = max(dot(L, N), 0.001);
            H = normalize(V + L);
        }
        NdotL = max(NdotL, 0.001);

        float VdotH = max(dot(V, H), 0.001);
        float NdotH = max(dot(N, H), 0.001);

        vec3 F = fresnelSchlick(VdotH, albedo);
        float G = geometrySmith(NdotV, NdotL, roughness);

        throughput = F * G * VdotH / (NdotV * NdotH);
    } else {
        vec3 L_local = sampleCosineHemisphere(r1, r2);
        L = normalize(tangentToWorld(L_local, N, T, B));
        throughput = albedo;
    }

    throughput = clamp(throughput, vec3(0.0), vec3(10.0));

    payloads.rayPayload.color *= throughput;
    payloads.rayPayload.depth += 1u;
    payloads.rayPayload.direction = L;

    traceRay(hitPos + N * 0.001, 0.01, L, 1000.0);
}
