#include "usd_loader.hpp"

#include <iostream>

#ifdef TRACEY_HAS_USD

#include "actor.hpp"
#include "camera.hpp"
#include "light.hpp"
#include "material_instance.hpp"
#include "scene_instance.hpp"
#include "scene_object.hpp"
#include "transform.hpp"
#include "usd_internal.hpp"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp> // glm::rotate / glm::radians (up-axis fix)

#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace tracey
{
    namespace
    {
        // USD GfMatrix4d is row-major / row-vector (p' = p·M); glm is
        // column-major / column-vector (p' = M·p). Copying element-for-element
        // with matching indices yields the transpose, which is exactly the
        // convention flip we want (translation lands in glm's 4th column).
        glm::mat4 toGlm(const GfMatrix4d &M)
        {
            glm::mat4 g(1.0f);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    g[i][j] = static_cast<float>(M[i][j]);
            return g;
        }

        // Conversion from the stage's up-axis to the engine's Y-up convention.
        // USD stages declare upAxis = "Y" or "Z" (Z is the default for many DCC
        // exports — Maya/Houdini/Pixar Kitchen Set). For a Z-up stage we rotate
        // the whole scene −90° about X so +Z maps to +Y: (x,y,z) → (x, z, −y).
        // Pre-multiplied into every prim's world transform (geometry, camera,
        // lights), so the scene comes in upright instead of laid on its side.
        glm::mat4 upAxisToYup(const UsdStageRefPtr &stage)
        {
            if (UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z)
                return glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f),
                                   glm::vec3(1.0f, 0.0f, 0.0f));
            return glm::mat4(1.0f);
        }

        // Quaternion → ZYX-intrinsic euler-degrees. Copied verbatim from
        // gltf_loader.cpp so USD peek lands rotations in the SAME convention
        // the editor's transform_sop / subnet / set_actor_rotation_euler use —
        // peek populates SOP `rotate_euler_deg` directly with no conversion at
        // the call site. Gimbal-locked near ±90° pitch (as Houdini lives with).
        Vec3 quatToEulerDegZYX(const glm::quat &q)
        {
            const glm::mat3 m = glm::mat3_cast(q);
            const float r02 = m[0][2];
            const float r00 = m[0][0];
            const float r01 = m[0][1];
            const float r12 = m[1][2];
            const float r22 = m[2][2];

            const float sy = -r02;
            const float cy = std::sqrt(r00 * r00 + r01 * r01);

            float rx, ry, rz;
            if (cy > 1e-6f)
            {
                rx = std::atan2(r12, r22);
                ry = std::atan2(sy, cy);
                rz = std::atan2(r01, r00);
            }
            else
            {
                const float r11 = m[1][1];
                const float r21 = m[2][1];
                rx = std::atan2(-r21, r11);
                ry = std::atan2(sy, cy);
                rz = 0.0f;
            }

            constexpr float kRad2Deg = 180.0f / 3.1415926535f;
            return Vec3(rx * kRad2Deg, ry * kRad2Deg, rz * kRad2Deg);
        }

        // Decompose a world matrix to TRS. When `prevEulerDeg` is non-null the
        // euler result is unwrapped to stay within ±180° of it per axis, so a
        // continuously rotating prim's per-frame decomposition doesn't jump
        // ±360° between adjacent samples (which would make playback spin).
        //
        // LIMITATION: the editor animates rotation as ZYX euler-degree channels,
        // and matrix→euler is singular at ry = ±90° (the middle-axis gimbal).
        // Translate/scale and X/Z-axis rotations bake cleanly at any angle;
        // rotations that cross ±90° about *world Y* can't be represented as a
        // continuous euler path and will tumble. The proper fix is quaternion
        // keyframe channels in the animation engine (a separate enhancement);
        // until then, euler is what the editor's own rotation keys use too.
        void decomposeTRS(const glm::mat4 &m, glm::vec3 &outT, glm::vec3 &outR,
                          glm::vec3 &outS, const glm::vec3 *prevEulerDeg)
        {
            glm::vec3 scale, translation, skew;
            glm::vec4 perspective;
            glm::quat rotation;
            if (!glm::decompose(m, scale, rotation, translation, skew, perspective))
            {
                outT = glm::vec3(0.0f);
                outR = glm::vec3(0.0f);
                outS = glm::vec3(1.0f);
                return;
            }
            outT = translation;
            outS = scale;
            glm::vec3 e = quatToEulerDegZYX(rotation);
            if (prevEulerDeg)
            {
                for (int i = 0; i < 3; ++i)
                {
                    while (e[i] - (*prevEulerDeg)[i] > 180.0f)  e[i] -= 360.0f;
                    while (e[i] - (*prevEulerDeg)[i] < -180.0f) e[i] += 360.0f;
                }
            }
            outR = e;
        }

        Transform transformFromMatrix(const glm::mat4 &m)
        {
            Transform t;
            glm::vec3 scale, translation, skew;
            glm::vec4 perspective;
            glm::quat rotation;
            if (glm::decompose(m, scale, rotation, translation, skew, perspective))
            {
                t.setPosition(translation);
                t.setRotation(rotation);
                t.setScale(scale);
            }
            return t;
        }

        // Follow a UsdPreviewSurface input's connection to a UsdUVTexture and
        // return its resolved image file path (empty when the input isn't
        // texture-connected). USD textures are external files; we hand the
        // path straight to SceneCompiler::loadTexture, which reads it from disk
        // via stb_image (no need to pre-decode into an embedded buffer the way
        // glTF does). One hop only — UV transforms / primvar readers upstream
        // of the UsdUVTexture are a follow-up.
        std::string connectedTextureFile(const UsdShadeShader &surface, const char *inputName)
        {
            UsdShadeInput in = surface.GetInput(TfToken(inputName));
            if (!in) return {};
            const UsdShadeSourceInfoVector sources = in.GetConnectedSources();
            if (sources.empty() || !sources[0].source) return {};
            UsdShadeShader tex(sources[0].source.GetPrim());
            if (!tex) return {};
            TfToken id;
            tex.GetIdAttr().Get(&id);
            if (id != TfToken("UsdUVTexture")) return {};
            UsdShadeInput fileIn = tex.GetInput(TfToken("file"));
            if (!fileIn) return {};
            SdfAssetPath asset;
            if (!fileIn.Get(&asset)) return {};
            std::string path = asset.GetResolvedPath();
            if (path.empty()) path = asset.GetAssetPath(); // unresolvable → authored path
            return path;
        }

        // Map a prim's bound UsdPreviewSurface to a MaterialInstance. Reads
        // constant input values/defaults AND follows texture-connected inputs
        // (diffuseColor/normal/emissive/occlusion/metallic-roughness) to their
        // UsdUVTexture file paths.
        MaterialInstance convertBoundMaterial(const UsdPrim &prim)
        {
            MaterialInstance m("pbr");

            // displayColor: USD's preview/fallback color for prims with no
            // bound shading material. Many real assets rely on it entirely —
            // the Pixar Kitchen Set colors all 1788 meshes via a constant
            // displayColor and binds NO materials + ships NO textures. Read it
            // first as the albedo (constant interp → one color; varying →
            // first sample) so such assets import in their intended colors
            // instead of flat grey. A real bound UsdPreviewSurface below wins.
            if (UsdGeomPrimvar dc = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken("displayColor")))
            {
                VtArray<GfVec3f> colors;
                if (dc.ComputeFlattened(&colors) && !colors.empty())
                    m.setAlbedo(Vec3(colors[0][0], colors[0][1], colors[0][2]));
            }

            // Resolve the bound material. ComputeBoundMaterial() (via
            // BindingsAtPrim) logs a per-prim warning when a prim has material
            // bindings but never applied the MaterialBindingAPI schema — common
            // in Omniverse / NVIDIA assets (e.g. Marbles), which set the
            // material:binding relationship directly. Use the full resolver only
            // when the API is actually applied; otherwise resolve the direct
            // binding relationship ourselves — no warning, and it covers the
            // legacy direct-binding case those assets use.
            UsdShadeMaterial mat;
            if (prim.HasAPI<UsdShadeMaterialBindingAPI>())
            {
                mat = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
            }
            else if (UsdRelationship rel = prim.GetRelationship(TfToken("material:binding")))
            {
                SdfPathVector targets;
                if (rel.GetForwardedTargets(&targets) && !targets.empty())
                    mat = UsdShadeMaterial(prim.GetStage()->GetPrimAtPath(targets[0]));
            }
            if (!mat) return m;

            // ── OmniPBR / OmniGlass (MDL) materials ──────────────────────────
            // NVIDIA Omniverse assets (Marbles, Old Attic, …) don't author a
            // UsdPreviewSurface; they reference an MDL module (OmniPBR/OmniGlass)
            // and expose its parameters as interface inputs on the Material prim
            // (e.g. inputs:diffuse_color_constant). We don't run MDL, but reading
            // those well-known constants + direct texture-asset inputs imports the
            // material in its authored look instead of flat grey. A real
            // UsdPreviewSurface (handled below) takes precedence.
            {
                // Omniverse authors OmniPBR/OmniGlass parameters in two styles:
                //  (a) as interface inputs on the Material prim — paint
                //      containers etc. read via mat.GetInput(...);
                //  (b) directly on the MDL Shader prim that outputs:mdl:surface
                //      points at — the marbles themselves.
                // Read BOTH: for each param prefer whichever prim actually
                // authored a value (Material interface first, then the shader).
                UsdShadeShader mdlShader =
                    mat.ComputeSurfaceSource(TfTokenVector{TfToken("mdl")});

                auto findInput = [&](const char *name) -> UsdShadeInput {
                    UsdShadeInput mi = mat.GetInput(TfToken(name));
                    if (mi && mi.GetAttr().HasAuthoredValue()) return mi;
                    if (mdlShader)
                    {
                        UsdShadeInput si = mdlShader.GetInput(TfToken(name));
                        if (si && si.GetAttr().HasAuthoredValue()) return si;
                    }
                    return mi; // possibly invalid — getters guard with `if (in)`
                };
                auto omF = [&](const char *name, float def) {
                    float v = def;
                    if (UsdShadeInput in = findInput(name)) in.Get(&v);
                    return v;
                };
                auto omB = [&](const char *name, bool def) {
                    bool v = def;
                    if (UsdShadeInput in = findInput(name)) in.Get(&v);
                    return v;
                };
                auto omC = [&](const char *name, const GfVec3f &def) {
                    GfVec3f v = def;
                    if (UsdShadeInput in = findInput(name)) in.Get(&v);
                    return v;
                };
                auto omAsset = [&](const char *name) -> std::string {
                    UsdShadeInput in = findInput(name);
                    if (!in) return {};
                    SdfAssetPath a;
                    if (!in.Get(&a)) return {};
                    std::string p = a.GetResolvedPath();
                    if (p.empty()) p = a.GetAssetPath();
                    return p;
                };

                // Identify the MDL by its sub-identifier (OmniPBR / OmniGlass),
                // falling back to the presence of signature inputs.
                TfToken subId;
                if (mdlShader)
                    if (UsdAttribute a = mdlShader.GetPrim().GetAttribute(
                            TfToken("info:mdl:sourceAsset:subIdentifier")))
                        a.Get(&subId);
                const bool isGlass =
                    subId == TfToken("OmniGlass") || (bool)findInput("glass_color");
                const bool isPBR = !isGlass &&
                    (subId == TfToken("OmniPBR") ||
                     (bool)findInput("diffuse_color_constant") ||
                     (bool)findInput("diffuse_texture"));

                if (isPBR)
                {
                    const GfVec3f dc = omC("diffuse_color_constant", GfVec3f(0.2f, 0.2f, 0.2f));
                    const GfVec3f tint = omC("diffuse_tint", GfVec3f(1.0f, 1.0f, 1.0f));
                    m.setAlbedo(Vec3(dc[0] * tint[0], dc[1] * tint[1], dc[2] * tint[2]));
                    m.setMetallic(omF("metallic_constant", 0.0f));
                    m.setRoughness(omF("reflection_roughness_constant", 0.5f));
                    if (omB("enable_emission", false))
                    {
                        const GfVec3f ec = omC("emissive_color", GfVec3f(0.0f, 0.0f, 0.0f));
                        m.setEmission(Vec3(ec[0], ec[1], ec[2]));
                        m.setFloat("emissionStrength", omF("emissive_intensity", 1.0f));
                    }
                    if (omB("enable_opacity", false))
                        m.setFloat("opacity", omF("opacity_constant", 1.0f));

                    auto omniTex = [&](const char *input, const char *slot) {
                        std::string f = omAsset(input);
                        if (!f.empty()) m.setTexture(slot, f);
                    };
                    omniTex("diffuse_texture", TEXTURE_ALBEDO);
                    omniTex("normalmap_texture", TEXTURE_NORMAL);
                    // ORM (occlusion/roughness/metallic) packed texture → MR slot;
                    // fall back to a roughness-only texture.
                    std::string mr = omAsset("ORM_texture");
                    if (mr.empty()) mr = omAsset("reflectionroughness_texture");
                    if (!mr.empty()) m.setTexture(TEXTURE_METALLIC_ROUGHNESS, mr);
                }
                else if (isGlass)
                {
                    // OmniGlass → refractive dielectric (transmission on).
                    const GfVec3f gc = omC("glass_color", GfVec3f(1.0f, 1.0f, 1.0f));
                    m.setAlbedo(Vec3(gc[0], gc[1], gc[2]));
                    m.setFloat("ior", omF("glass_ior", 1.5f));
                    m.setRoughness(omF("frosting_roughness", 0.0f));
                    m.setMetallic(0.0f);
                    m.setFloat("transmission", 1.0f);
                }
            }

            UsdShadeShader surface = mat.ComputeSurfaceSource();
            if (!surface) return m;
            // Only run the UsdPreviewSurface reads when the surface really is one.
            // For an MDL (OmniPBR/Glass) surface the inputs use different names and
            // the getF/getC defaults below would clobber the values set above.
            {
                TfToken sid;
                surface.GetIdAttr().Get(&sid);
                if (sid != TfToken("UsdPreviewSurface")) return m;
            }

            auto getF = [&](const char *name, float def) {
                float v = def;
                if (UsdShadeInput in = surface.GetInput(TfToken(name))) in.Get(&v);
                return v;
            };
            auto getC = [&](const char *name, const GfVec3f &def) {
                GfVec3f v = def;
                if (UsdShadeInput in = surface.GetInput(TfToken(name))) in.Get(&v);
                return Vec3(v[0], v[1], v[2]);
            };

            m.setAlbedo(getC("diffuseColor", GfVec3f(0.8f, 0.8f, 0.8f)));
            m.setMetallic(getF("metallic", 0.0f));
            m.setRoughness(getF("roughness", 0.5f));
            m.setFloat("ior", getF("ior", 1.5f));
            m.setFloat("opacity", getF("opacity", 1.0f));
            m.setEmission(getC("emissiveColor", GfVec3f(0.0f, 0.0f, 0.0f)));
            m.setFloat("clearcoat", getF("clearcoat", 0.0f));
            m.setFloat("clearcoatRoughness", getF("clearcoatRoughness", 0.0f));

            // Texture-connected inputs → file paths on the MaterialInstance.
            // SceneCompiler::convertMaterial reads these slots, loads the file,
            // and binds it into the GPUMaterial's texture index.
            auto setTex = [&](const char *input, const char *slot) {
                std::string f = connectedTextureFile(surface, input);
                if (!f.empty()) m.setTexture(slot, f);
            };
            setTex("diffuseColor", TEXTURE_ALBEDO);
            setTex("normal", TEXTURE_NORMAL);
            setTex("emissiveColor", TEXTURE_EMISSIVE);
            setTex("occlusion", TEXTURE_OCCLUSION);
            // metallic + roughness usually share one ORM-style texture (the
            // renderer's MR slot samples G=roughness, B=metallic — matching
            // UsdUVTexture ORM packing). Prefer whichever input is connected.
            {
                std::string mr = connectedTextureFile(surface, "roughness");
                if (mr.empty()) mr = connectedTextureFile(surface, "metallic");
                if (!mr.empty()) m.setTexture(TEXTURE_METALLIC_ROUGHNESS, mr);
            }
            return m;
        }

        // Map a UsdLux light prim (already known to be one of the supported
        // types) to an engine Light. Common attrs come from UsdLuxLightAPI;
        // per-type extents from the concrete schema. The actor transform the
        // caller sets supplies position (origin) + direction (-Z), matching
        // both USD's light convention and the SceneCompiler's encoding.
        void convertLight(const UsdPrim &prim, Light &out)
        {
            UsdLuxLightAPI api(prim);
            float intensity = 1.0f, exposure = 0.0f;
            api.GetIntensityAttr().Get(&intensity);
            api.GetExposureAttr().Get(&exposure);
            out.intensity = intensity * std::pow(2.0f, exposure);
            GfVec3f color(1.0f, 1.0f, 1.0f);
            api.GetColorAttr().Get(&color);
            out.color = Vec3(color[0], color[1], color[2]);

            if (prim.IsA<UsdLuxDistantLight>())
            {
                out.type = LightType::Distant;
            }
            else if (prim.IsA<UsdLuxSphereLight>())
            {
                out.type = LightType::Point;
                float r = 0.0f;
                UsdLuxSphereLight(prim).GetRadiusAttr().Get(&r);
                out.radius = r;
            }
            else if (prim.IsA<UsdLuxRectLight>())
            {
                out.type = LightType::Area;
                float w = 1.0f, h = 1.0f;
                UsdLuxRectLight rect(prim);
                rect.GetWidthAttr().Get(&w);
                rect.GetHeightAttr().Get(&h);
                out.size = Vec2(w, h);
            }
            else // UsdLuxDomeLight
            {
                out.type = LightType::Dome;
                SdfAssetPath tex;
                if (UsdLuxDomeLight(prim).GetTextureFileAttr().Get(&tex) &&
                    !tex.GetResolvedPath().empty())
                    out.hdriPath = tex.GetResolvedPath();
            }
        }

        bool isSupportedLight(const UsdPrim &prim)
        {
            return prim.IsA<UsdLuxDistantLight>() || prim.IsA<UsdLuxSphereLight>() ||
                   prim.IsA<UsdLuxRectLight>() || prim.IsA<UsdLuxDomeLight>();
        }

        // Triangulate a UsdGeomMesh into a SceneObject. Returns false if the
        // mesh has no usable geometry. Handles USD primvar interpolation
        // (constant / uniform / vertex / varying / faceVarying) for `st` (UV)
        // and normals: when either is face-varying or uniform, the mesh is
        // DE-INDEXED — one output vertex per triangle corner, each carrying its
        // own UV/normal — because a renderer wanting one attribute per vertex
        // can't share a point that has different UVs/normals on different
        // faces. This is what makes production assets (Pixar Kitchen Set etc.,
        // whose `st` + normals are face-varying) texture + shade correctly.
        // Meshes with no `st` and no authored normals keep the cheap shared-
        // point path (the engine computes face normals).
        bool convertMesh(const UsdGeomMesh &mesh, SceneObject &out)
        {
            VtArray<GfVec3f> points;
            VtArray<int> faceCounts, faceIndices;
            mesh.GetPointsAttr().Get(&points);
            mesh.GetFaceVertexCountsAttr().Get(&faceCounts);
            mesh.GetFaceVertexIndicesAttr().Get(&faceIndices);
            if (points.empty() || faceCounts.empty() || faceIndices.empty()) return false;

            // `st` (UV) — ComputeFlattened resolves indexed primvars to a plain
            // array sized per its interpolation.
            UsdGeomPrimvar stPv = UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(TfToken("st"));
            VtArray<GfVec2f> st;
            TfToken stInterp;
            const bool hasSt = stPv && stPv.ComputeFlattened(&st) && !st.empty();
            if (hasSt) stInterp = stPv.GetInterpolation();

            // Normals (built-in attr, not a primvar) + their interpolation.
            VtArray<GfVec3f> normals;
            TfToken nInterp;
            const bool hasN = mesh.GetNormalsAttr().Get(&normals) && !normals.empty();
            if (hasN) nInterp = mesh.GetNormalsInterpolation();

            // Resolve a primvar value index for a given face / face-vertex /
            // point per the interpolation token.
            auto indexFor = [&](const TfToken &interp, int faceIdx,
                                size_t fvGlobal, int pointIdx, size_t n) -> size_t {
                size_t i;
                if (interp == UsdGeomTokens->constant)        i = 0;
                else if (interp == UsdGeomTokens->uniform)    i = static_cast<size_t>(faceIdx);
                else if (interp == UsdGeomTokens->faceVarying) i = fvGlobal;
                else /* vertex / varying */                    i = static_cast<size_t>(pointIdx);
                return (i < n) ? i : 0;
            };

            const bool deindex = hasSt || hasN;

            if (!deindex)
            {
                // Cheap path: shared points + fan indices; engine computes face
                // normals; no UVs.
                std::vector<Vec3> positions(points.size());
                for (size_t i = 0; i < points.size(); ++i)
                    positions[i] = Vec3(points[i][0], points[i][1], points[i][2]);
                std::vector<uint32_t> indices;
                size_t offset = 0;
                for (int c : faceCounts)
                {
                    if (c >= 3 && offset + c <= faceIndices.size())
                        for (int k = 1; k + 1 < c; ++k)
                        {
                            indices.push_back(static_cast<uint32_t>(faceIndices[offset + 0]));
                            indices.push_back(static_cast<uint32_t>(faceIndices[offset + k]));
                            indices.push_back(static_cast<uint32_t>(faceIndices[offset + k + 1]));
                        }
                    offset += static_cast<size_t>(c < 0 ? 0 : c);
                }
                if (indices.empty()) return false;
                out.setPositions(std::move(positions));
                out.setIndices(std::move(indices));
                return true;
            }

            // De-index: one output vertex per triangle corner.
            std::vector<Vec3> outPos;
            std::vector<Vec2> outUv;
            std::vector<Vec3> outNrm;
            std::vector<uint32_t> outIdx;
            size_t fvOffset = 0;
            for (int faceIdx = 0; faceIdx < static_cast<int>(faceCounts.size()); ++faceIdx)
            {
                const int c = faceCounts[faceIdx];
                if (c >= 3 && fvOffset + static_cast<size_t>(c) <= faceIndices.size())
                {
                    for (int k = 1; k + 1 < c; ++k)
                    {
                        const int corner[3] = {0, k, k + 1};
                        for (int ci = 0; ci < 3; ++ci)
                        {
                            const size_t fvGlobal = fvOffset + static_cast<size_t>(corner[ci]);
                            const int pointIdx = faceIndices[fvGlobal];
                            const auto &p = points[pointIdx];
                            outPos.emplace_back(p[0], p[1], p[2]);
                            if (hasSt)
                            {
                                const size_t i = indexFor(stInterp, faceIdx, fvGlobal, pointIdx, st.size());
                                outUv.emplace_back(st[i][0], st[i][1]);
                            }
                            if (hasN)
                            {
                                const size_t i = indexFor(nInterp, faceIdx, fvGlobal, pointIdx, normals.size());
                                outNrm.emplace_back(normals[i][0], normals[i][1], normals[i][2]);
                            }
                            outIdx.push_back(static_cast<uint32_t>(outPos.size() - 1));
                        }
                    }
                }
                fvOffset += static_cast<size_t>(c < 0 ? 0 : c);
            }
            if (outIdx.empty()) return false;

            out.setPositions(std::move(outPos));
            out.setIndices(std::move(outIdx));
            if (hasSt) out.setUvs(std::move(outUv));
            if (hasN) out.setNormals(std::move(outNrm));
            return true;
        }

        // One prototype mesh: its shared SceneObject name, its transform within
        // the prototype root, and its bound material. Shared by both instancing
        // paths (UsdGeomPointInstancer + native scenegraph instancing).
        struct ProtoMesh
        {
            std::string objName;
            glm::mat4 relToProto;
            MaterialInstance material;
        };

        // Gather every mesh under `protoRoot` into SceneObjects (added ONCE,
        // keyed by `keyPrefix`::<path> so all instances of this prototype share
        // the geometry → one BLAS each). Each entry also records the mesh's
        // transform relative to the prototype root (so multi-mesh / offset
        // prototypes instance correctly) + its material. Instance proxies are
        // traversed so a prototype that itself contains nested instances still
        // yields its geometry.
        std::vector<ProtoMesh> gatherPrototypeMeshes(const UsdPrim &protoRoot,
                                                     Scene &scene,
                                                     const std::string &keyPrefix)
        {
            std::vector<ProtoMesh> out;
            if (!protoRoot) return out;
            const glm::mat4 invWp = glm::inverse(toGlm(
                UsdGeomXformable(protoRoot).ComputeLocalToWorldTransform(UsdTimeCode::Default())));
            for (const UsdPrim &m : UsdPrimRange(protoRoot, UsdTraverseInstanceProxies()))
            {
                UsdGeomMesh mesh(m);
                if (!mesh) continue;
                const std::string objName = keyPrefix + "::" + m.GetPath().GetString();
                if (!scene.hasObject(objName))
                {
                    auto obj = std::make_unique<SceneObject>();
                    if (!convertMesh(mesh, *obj)) continue;
                    obj->setName(objName);
                    scene.addObject(objName, std::move(obj));
                }
                ProtoMesh pm;
                pm.objName = objName;
                pm.relToProto = invWp * toGlm(
                    UsdGeomXformable(m).ComputeLocalToWorldTransform(UsdTimeCode::Default()));
                pm.material = convertBoundMaterial(m);
                out.push_back(std::move(pm));
            }
            return out;
        }

        // Expand a UsdGeomPointInstancer into shared prototype geometry +
        // per-instance placements: one Actor holding a SceneInstance per
        // (instance × prototype-mesh), each carrying that instance's world
        // transform. The SceneCompiler builds one BLAS per prototype mesh and
        // one TLAS instance per placement — true hardware instancing. Returns
        // the number of placements created. Static (default-time) only;
        // per-instance animation + invisibleIds are follow-ups.
        int convertPointInstancer(const UsdGeomPointInstancer &pi, Scene &scene,
                                  const glm::mat4 &upM)
        {
            const UsdPrim prim = pi.GetPrim();
            SdfPathVector protoPaths;
            pi.GetPrototypesRel().GetTargets(&protoPaths);
            if (protoPaths.empty()) return 0;

            VtArray<int> protoIndices;
            pi.GetProtoIndicesAttr().Get(&protoIndices);
            if (protoIndices.empty()) return 0;

            // IncludeProtoXform: bake each prototype root's own transform into
            // the placement. IgnoreMask: keep the result 1:1 with protoIndices.
            VtArray<GfMatrix4d> xforms;
            if (!pi.ComputeInstanceTransformsAtTime(
                    &xforms, UsdTimeCode::Default(), UsdTimeCode::Default(),
                    UsdGeomPointInstancer::IncludeProtoXform,
                    UsdGeomPointInstancer::IgnoreMask))
                return 0;
            if (xforms.size() != protoIndices.size()) return 0;

            UsdStageWeakPtr stage = prim.GetStage();
            const glm::mat4 Winstancer = toGlm(
                UsdGeomXformable(prim).ComputeLocalToWorldTransform(UsdTimeCode::Default()));

            std::vector<std::vector<ProtoMesh>> protoMeshes(protoPaths.size());
            for (size_t k = 0; k < protoPaths.size(); ++k)
                protoMeshes[k] = gatherPrototypeMeshes(
                    stage->GetPrimAtPath(protoPaths[k]), scene,
                    prim.GetPath().GetString() + "::proto" + std::to_string(k));

            Actor *actor = scene.createActor();
            actor->setName("instancer:" + prim.GetPath().GetString());

            int count = 0;
            for (size_t i = 0; i < protoIndices.size(); ++i)
            {
                const int k = protoIndices[i];
                if (k < 0 || k >= static_cast<int>(protoMeshes.size())) continue;
                const glm::mat4 Xi = toGlm(xforms[i]);
                for (const auto &pm : protoMeshes[k])
                {
                    SceneInstance inst(pm.objName, pm.material);
                    inst.setLocalTransform(
                        transformFromMatrix(upM * Winstancer * Xi * pm.relToProto));
                    actor->addInstance(std::move(inst));
                    ++count;
                }
            }
            return count;
        }
    }

    bool UsdLoader::available() { return true; }

    std::unique_ptr<Scene> UsdLoader::loadFromFile(const std::string &path)
    {
        UsdStageRefPtr stage = UsdStage::Open(path);
        if (!stage)
        {
            std::cerr << "[usd] failed to open stage: " << path << std::endl;
            return nullptr;
        }
        return convertStageToScene(stage, path);
    }

    // Shared conversion body: an already-open, composed stage → Scene. Reused by
    // loadFromFile (above) and StageDocument::toScene (the live-stage render bridge).
    std::unique_ptr<Scene> convertStageToScene(const UsdStageRefPtr &stage,
                                               const std::string &sourceLabel,
                                               const UsdTimeCode &time)
    {
        auto scene = std::make_unique<Scene>();
        const glm::mat4 upM = upAxisToYup(stage); // Z-up → Y-up when needed
        int meshes = 0, lights = 0, instances = 0;
        bool gotCamera = false;

        // Pre-pass: gather every PointInstancer's prototype roots. The prototype
        // geometry lives in the stage like any other prim, so we must NOT import
        // it as standalone meshes — it's instanced via the PointInstancer below.
        SdfPathVector protoRoots;
        for (const UsdPrim &p : stage->Traverse())
            if (p.IsA<UsdGeomPointInstancer>())
            {
                SdfPathVector targets;
                UsdGeomPointInstancer(p).GetPrototypesRel().GetTargets(&targets);
                for (const SdfPath &t : targets) protoRoots.push_back(t);
            }
        auto underPrototype = [&protoRoots](const SdfPath &path) {
            for (const SdfPath &r : protoRoots)
                if (path == r || path.HasPrefix(r)) return true;
            return false;
        };

        // Shared prototype geometry for native (scenegraph) instancing, keyed by
        // prototype path so all instances of the same master reuse one BLAS set.
        std::unordered_map<std::string, std::vector<ProtoMesh>> protoCache;

        for (const UsdPrim &prim : stage->Traverse())
        {
            // Prototype geometry is consumed by its PointInstancer, not imported
            // standalone.
            if (underPrototype(prim.GetPath())) continue;

            // PointInstancer → shared prototypes + per-instance placements.
            if (prim.IsA<UsdGeomPointInstancer>())
            {
                instances += convertPointInstancer(UsdGeomPointInstancer(prim), *scene, upM);
                continue;
            }

            // Native scenegraph instancing: an `instanceable` prim that
            // references shared content (the Pixar Kitchen Set's instanced
            // variant is 425 of these). USD composes it into a prototype;
            // Traverse() doesn't descend into the instance's children, so we
            // pull geometry from the prototype, share it across all instances of
            // that master (one BLAS set), and place each by its world transform.
            if (prim.IsInstance())
            {
                UsdPrim proto = prim.GetPrototype();
                if (proto)
                {
                    const std::string key = proto.GetPath().GetString();
                    auto it = protoCache.find(key);
                    if (it == protoCache.end())
                        it = protoCache.emplace(key, gatherPrototypeMeshes(proto, *scene, key)).first;
                    if (!it->second.empty())
                    {
                        const glm::mat4 instWorld = upM * toGlm(
                            UsdGeomXformable(prim).ComputeLocalToWorldTransform(time));
                        Actor *actor = scene->createActor();
                        actor->setName("instance:" + prim.GetPath().GetString());
                        for (const auto &pm : it->second)
                        {
                            SceneInstance inst(pm.objName, pm.material);
                            inst.setLocalTransform(transformFromMatrix(instWorld * pm.relToProto));
                            actor->addInstance(std::move(inst));
                            ++instances;
                        }
                    }
                }
                continue;
            }

            const glm::mat4 world = upM * toGlm(
                UsdGeomXformable(prim).ComputeLocalToWorldTransform(time));

            // First camera found becomes the scene camera.
            if (!gotCamera && prim.IsA<UsdGeomCamera>())
            {
                Camera camera;
                camera.setPosition(Vec3(world[3]));
                glm::mat3 rot(world);
                rot[0] = glm::normalize(rot[0]);
                rot[1] = glm::normalize(rot[1]);
                rot[2] = glm::normalize(rot[2]);
                camera.setRotation(glm::quat_cast(rot));
                const float vfov = static_cast<float>(
                    UsdGeomCamera(prim).GetCamera(time)
                        .GetFieldOfView(GfCamera::FOVVertical));
                if (vfov > 0.0f) camera.setFov(vfov);
                scene->setCamera(camera);
                gotCamera = true;
                continue;
            }

            // UsdLux light → an actor with a Light component + the prim's
            // world transform (which supplies position + -Z direction).
            if (isSupportedLight(prim))
            {
                Light light;
                convertLight(prim, light);
                Actor *actor = scene->createActor();
                actor->setName(prim.GetPath().GetString());
                actor->setTransform(transformFromMatrix(world));
                actor->setLight(light);
                ++lights;
                continue;
            }

            UsdGeomMesh mesh(prim);
            if (!mesh) continue;

            auto obj = std::make_unique<SceneObject>();
            if (!convertMesh(mesh, *obj)) continue;
            const std::string name = prim.GetPath().GetString();
            obj->setName(name);
            scene->addObject(name, std::move(obj));

            // One actor per mesh prim, world transform baked on (hierarchy
            // preservation is a follow-up; this renders correctly today).
            Actor *actor = scene->createActor();
            actor->setName(name);
            actor->setTransform(transformFromMatrix(world));

            SceneInstance inst(name);
            inst.setMaterial(convertBoundMaterial(prim));
            actor->addInstance(std::move(inst));
            ++meshes;
        }

        std::cout << "[usd] imported " << meshes << " mesh(es), " << instances
                  << " instance(s), " << lights << " light(s)"
                  << (gotCamera ? ", 1 camera" : "") << " from " << sourceLabel << std::endl;
        return scene;
    }

    namespace
    {
        // Process-wide parsed-stage cache (mirrors GltfLoader's). Stores
        // shared_ptr<const Scene> so the SOP cook + apply_emitted material
        // re-resolution share one parse. Guarded by a mutex — cooks run on a
        // worker thread.
        std::mutex g_cacheMutex;
        std::unordered_map<std::string, std::shared_ptr<const Scene>> g_cache;
    }

    std::shared_ptr<const Scene> UsdLoader::loadFromFileCached(const std::string &path)
    {
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            auto it = g_cache.find(path);
            if (it != g_cache.end()) return it->second;
        }
        // Parse outside the lock (USD open can be slow); first writer wins on
        // the rare race — both produce equivalent scenes.
        std::shared_ptr<const Scene> parsed = loadFromFile(path);
        if (!parsed) return nullptr;
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto [it, inserted] = g_cache.emplace(path, parsed);
        return it->second;
    }

    void UsdLoader::invalidateCache(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache.erase(path);
    }

    std::vector<UsdLoader::HierarchyNode> UsdLoader::peekHierarchy(const std::string &path,
                                                                  StageTimeInfo *outInfo)
    {
        std::vector<HierarchyNode> roots;
        UsdStageRefPtr stage = UsdStage::Open(path);
        if (!stage)
        {
            std::cerr << "[usd] peek failed to open stage: " << path << std::endl;
            return roots;
        }

        StageTimeInfo info;
        const double tcps = stage->GetTimeCodesPerSecond();
        info.timeCodesPerSecond = (tcps > 0.0) ? tcps : 24.0;
        info.startTimeCode = stage->GetStartTimeCode();
        info.endTimeCode = stage->GetEndTimeCode();
        const glm::mat4 upM = upAxisToYup(stage); // must match loadFromFile

        // PointInstancer prototype roots — their meshes are instanced, not
        // standalone, so the SOP geometry import must skip them (mirrors
        // loadFromFile). Native-instanced (instanceable) prims are naturally
        // skipped: Traverse() doesn't descend into them, and they're not meshes.
        SdfPathVector protoRoots;
        for (const UsdPrim &p : stage->Traverse())
            if (p.IsA<UsdGeomPointInstancer>())
            {
                SdfPathVector targets;
                UsdGeomPointInstancer(p).GetPrototypesRel().GetTargets(&targets);
                for (const SdfPath &t : targets) protoRoots.push_back(t);
            }
        auto underPrototype = [&protoRoots](const SdfPath &path) {
            for (const SdfPath &r : protoRoots)
                if (path == r || path.HasPrefix(r)) return true;
            return false;
        };

        // Flat first slice: one root node per mesh prim, carrying its world
        // transform (decomposed to TRS). meshObjectNames = the prim's full path
        // — the exact key loadFromFile registers the SceneObject under, so the
        // usd_import SOP's getObject() lookup hits. When the prim's world
        // transform is animated (its own or an ancestor's xformOps carry time
        // samples) we also emit the per-sample TRS so the importer bakes
        // keyframes. Xform-nesting preservation is a follow-up (still flat).
        for (const UsdPrim &prim : stage->Traverse())
        {
            if (underPrototype(prim.GetPath())) continue; // instanced via its PointInstancer
            UsdGeomMesh mesh(prim);
            if (!mesh) continue;

            // Skip prims with no usable geometry (mirrors convertMesh's gate)
            // so peek never emits a name the cook can't resolve.
            VtArray<GfVec3f> points;
            VtArray<int> faceCounts;
            mesh.GetPointsAttr().Get(&points);
            mesh.GetFaceVertexCountsAttr().Get(&faceCounts);
            if (points.empty() || faceCounts.empty()) continue;

            HierarchyNode node;
            node.name = prim.GetName().GetString();
            if (node.name.empty()) node.name = prim.GetPath().GetString();
            node.meshObjectNames.push_back(prim.GetPath().GetString());

            // Static (default-time) transform — the fallback + the constant
            // value the channels override during playback.
            UsdGeomXformable xformable(prim);
            decomposeTRS(upM * toGlm(xformable.ComputeLocalToWorldTransform(UsdTimeCode::Default())),
                         node.translate, node.rotateEulerDeg, node.scale, nullptr);

            // World transform = product up the chain, so collect the union of
            // xform time samples on the prim AND its ancestors (ancestor
            // animation moves the child too). Empty union → static prim.
            std::set<double> times;
            for (UsdPrim p = prim; p && p.GetPath() != SdfPath::AbsoluteRootPath();
                 p = p.GetParent())
            {
                UsdGeomXformable xf(p);
                if (!xf) continue;
                std::vector<double> ts;
                if (xf.GetTimeSamples(&ts))
                    for (double t : ts) times.insert(t);
            }

            if (!times.empty())
            {
                node.trsSamples.reserve(times.size());
                glm::vec3 prevR = node.rotateEulerDeg;
                bool first = true;
                for (double t : times)
                {
                    TrsSample s;
                    s.timeCode = t;
                    decomposeTRS(upM * toGlm(xformable.ComputeLocalToWorldTransform(UsdTimeCode(t))),
                                 s.translate, s.rotateEulerDeg, s.scale,
                                 first ? nullptr : &prevR);
                    prevR = s.rotateEulerDeg;
                    if (first)
                    {
                        // Anchor the constant value to the first sample.
                        node.translate = s.translate;
                        node.rotateEulerDeg = s.rotateEulerDeg;
                        node.scale = s.scale;
                        first = false;
                    }
                    node.trsSamples.push_back(s);
                }
                info.hasAnimation = true;
            }

            roots.push_back(std::move(node));
        }

        // Derive a range from the samples when the stage didn't author one.
        if (info.hasAnimation && info.endTimeCode <= info.startTimeCode)
        {
            double mn = 1e30, mx = -1e30;
            for (const auto &n : roots)
                for (const auto &s : n.trsSamples)
                {
                    mn = std::min(mn, s.timeCode);
                    mx = std::max(mx, s.timeCode);
                }
            if (mx >= mn) { info.startTimeCode = mn; info.endTimeCode = mx; }
        }

        if (outInfo) *outInfo = info;
        std::cout << "[usd] peek: " << roots.size() << " mesh prim(s)"
                  << (info.hasAnimation ? " (animated)" : "") << " in " << path << std::endl;
        return roots;
    }
}

#else // !TRACEY_HAS_USD

namespace tracey
{
    bool UsdLoader::available() { return false; }

    std::unique_ptr<Scene> UsdLoader::loadFromFile(const std::string &path)
    {
        std::cerr << "[usd] build has no OpenUSD support; cannot load " << path
                  << " (run scripts/bootstrap_deps.sh and rebuild)." << std::endl;
        return nullptr;
    }

    std::shared_ptr<const Scene> UsdLoader::loadFromFileCached(const std::string &)
    {
        return nullptr;
    }

    void UsdLoader::invalidateCache(const std::string &) {}

    std::vector<UsdLoader::HierarchyNode> UsdLoader::peekHierarchy(const std::string &,
                                                                  StageTimeInfo *)
    {
        return {};
    }
}

#endif
