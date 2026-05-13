#version 450

// Studio-preview shading for the viewport rasterizer.
//
// Inputs:
//   fragWorldPos  — interpolated world-space position. The triangle's flat
//                   normal is derived from screen-space derivatives below
//                   because the vertex input layout doesn't carry per-vertex
//                   normals; that gives a faceted look but avoids a third
//                   vertex binding and keeps the rasterizer's input
//                   description identical to what SceneCompiler ships.
//   fragBaseColor — material's base color × per-vertex Cd (vertex shader
//                   already multiplied them). For glTF-material instances
//                   this comes from the material albedo; for SOP-cooked
//                   actors with an attached material graph, SceneCompiler
//                   extracts a preview color from the graph's WriteAlbedo
//                   output and stuffs it into GPUMaterial.baseColor — see
//                   extractGraphPreviewColor() in scene_compiler.cpp.
//
// Lighting model — three lights baked into the shader, so the viewport
// looks like a "studio" preview regardless of where the camera is:
//   1. Hemisphere ambient: lerp between a sky tint (normal up) and a
//      warmer ground tint (normal down). Gives soft directional cues
//      even on completely unlit silhouettes.
//   2. Key light: camera-relative top-right, the primary shaper.
//   3. Fill light: camera-relative lower-left, dimmer and warmer.
//   4. Rim: soft fresnel kicker that lifts silhouettes against a dark
//      background, so geometry never blends into the clear color.
//
// All lighting is computed in tangent-to-view space (we treat the world
// as if the camera is at the origin looking down -Z) so the lights track
// the camera. That's wrong for path-traced rendering — but for a
// viewport preview it means objects always look modeled regardless of
// the user's camera orbit.

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec4 fragBaseColor;

layout(location = 0) out vec4 outColor;

// Build a flat per-pixel normal from screen-space position derivatives.
// CCW triangles under our +Y-flipped projection produce a viewer-facing
// normal with cross(dFdy, dFdx) — see the original `position_only.frag`
// for the derivation.
vec3 faceNormal(vec3 worldPos) {
    vec3 dx = dFdx(worldPos);
    vec3 dy = dFdy(worldPos);
    return normalize(cross(dy, dx));
}

void main() {
    vec3 N = faceNormal(fragWorldPos);
    vec3 V = vec3(0.0, 0.0, 1.0);  // view direction in NDC-ish camera space

    // Hemisphere ambient. The "up" reference is also camera-space +Y
    // so it tracks the camera; that's what makes an upright object
    // always read as "lit from above" regardless of viewing angle.
    vec3 skyColor    = vec3(0.55, 0.62, 0.72);  // cool sky
    vec3 groundColor = vec3(0.28, 0.24, 0.22);  // warm ground
    float upDot = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ambient = mix(groundColor, skyColor, upDot);

    // Key light: camera-relative top-right.
    vec3 keyDir   = normalize(vec3(0.5, 0.7, 0.5));
    vec3 keyColor = vec3(1.0, 0.96, 0.92);
    float keyNdotL = max(dot(N, keyDir), 0.0);

    // Soft Blinn-Phong specular kicker for the key light. Tight lobe
    // (exponent 32) keeps it from looking plasticky. Drives a single
    // highlight that helps the eye disambiguate curved vs. flat
    // surfaces.
    vec3 keyH = normalize(keyDir + V);
    float keySpec = pow(max(dot(N, keyH), 0.0), 32.0) * 0.25;

    // Fill: opposite-side, dimmer + warmer. Lifts the unlit side
    // without flattening the key contrast.
    vec3 fillDir   = normalize(vec3(-0.6, 0.1, 0.6));
    vec3 fillColor = vec3(0.62, 0.55, 0.48) * 0.45;
    float fillNdotL = max(dot(N, fillDir), 0.0);

    // Soft fresnel rim to separate silhouettes from the clear color.
    // Tuned to be subtle in the interior, punchy at grazing angles.
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 4.0);
    vec3 rimColor = vec3(0.65, 0.75, 0.85) * fresnel * 0.35;

    // Albedo from the vertex stage (push-constant baseColor × per-vertex Cd).
    // Clamp to non-negative — common debug viz (e.g. "Cd = P") feeds raw
    // world-space positions, which can be < 0 on one side of the origin.
    // Reinhard tonemap + pow(., 1/2.2) below produce NaN for negative
    // inputs, which most drivers render as black or undefined garbage.
    // Saturating to >=0 lets the positive half of a debug Cd still show
    // through, and matches viewport conventions in DCC tools.
    vec3 albedo = max(fragBaseColor.rgb, vec3(0.0));

    // Compose: hemisphere ambient × albedo (the "always-on" floor),
    // plus diffuse contributions from key + fill, plus the spec and
    // rim sitting on top (those don't tint with albedo so they stay
    // chromatic regardless of material color).
    vec3 color = albedo * ambient
               + albedo * keyColor  * keyNdotL
               + albedo * fillColor * fillNdotL
               + keyColor * keySpec
               + rimColor;

    // Reinhard tone map keeps highlights from clipping when a bright
    // material + key + rim stack at silhouette edges; gamma 2.2 for
    // the LDR swapchain. Both steps now operate on guaranteed
    // non-negative input thanks to the albedo clamp above plus the
    // bounded lighting contributions.
    color = max(color, vec3(0.0));
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, fragBaseColor.a);
}
