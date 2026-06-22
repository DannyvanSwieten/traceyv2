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

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

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

        // Map a prim's bound UsdPreviewSurface to a MaterialInstance. Reads
        // constant input values/defaults; texture-connected inputs are a
        // follow-up (the value/fallback is used until then).
        MaterialInstance convertBoundMaterial(const UsdPrim &prim)
        {
            MaterialInstance m("pbr");
            UsdShadeMaterial mat = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
            if (!mat) return m;
            UsdShadeShader surface = mat.ComputeSurfaceSource();
            if (!surface) return m;

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
        // mesh has no usable geometry. Per-vertex normals + `st` only (face-
        // varying primvars are a follow-up; the engine computes face normals
        // and defaults UVs when absent).
        bool convertMesh(const UsdGeomMesh &mesh, SceneObject &out)
        {
            VtArray<GfVec3f> points;
            VtArray<int> faceCounts, faceIndices;
            mesh.GetPointsAttr().Get(&points);
            mesh.GetFaceVertexCountsAttr().Get(&faceCounts);
            mesh.GetFaceVertexIndicesAttr().Get(&faceIndices);
            if (points.empty() || faceCounts.empty() || faceIndices.empty()) return false;

            std::vector<Vec3> positions(points.size());
            for (size_t i = 0; i < points.size(); ++i)
                positions[i] = Vec3(points[i][0], points[i][1], points[i][2]);

            // Fan-triangulate each polygon over the shared point indices.
            std::vector<uint32_t> indices;
            size_t offset = 0;
            for (int c : faceCounts)
            {
                if (c >= 3 && offset + c <= faceIndices.size())
                {
                    for (int k = 1; k + 1 < c; ++k)
                    {
                        indices.push_back(static_cast<uint32_t>(faceIndices[offset + 0]));
                        indices.push_back(static_cast<uint32_t>(faceIndices[offset + k]));
                        indices.push_back(static_cast<uint32_t>(faceIndices[offset + k + 1]));
                    }
                }
                offset += static_cast<size_t>(c < 0 ? 0 : c);
            }
            if (indices.empty()) return false;

            out.setPositions(std::move(positions));
            out.setIndices(std::move(indices));

            VtArray<GfVec3f> normals;
            if (mesh.GetNormalsAttr().Get(&normals) && normals.size() == points.size())
            {
                std::vector<Vec3> n(normals.size());
                for (size_t i = 0; i < normals.size(); ++i)
                    n[i] = Vec3(normals[i][0], normals[i][1], normals[i][2]);
                out.setNormals(std::move(n));
            }

            UsdGeomPrimvar st = UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(TfToken("st"));
            if (st)
            {
                VtArray<GfVec2f> uvs;
                if (st.Get(&uvs) && uvs.size() == points.size())
                {
                    std::vector<Vec2> u(uvs.size());
                    for (size_t i = 0; i < uvs.size(); ++i)
                        u[i] = Vec2(uvs[i][0], uvs[i][1]);
                    out.setUvs(std::move(u));
                }
            }
            return true;
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

        auto scene = std::make_unique<Scene>();
        int meshes = 0, lights = 0;
        bool gotCamera = false;
        for (const UsdPrim &prim : stage->Traverse())
        {
            const glm::mat4 world = toGlm(
                UsdGeomXformable(prim).ComputeLocalToWorldTransform(UsdTimeCode::Default()));

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
                    UsdGeomCamera(prim).GetCamera(UsdTimeCode::Default())
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

        std::cout << "[usd] imported " << meshes << " mesh(es), " << lights << " light(s)"
                  << (gotCamera ? ", 1 camera" : "") << " from " << path << std::endl;
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

        // Flat first slice: one root node per mesh prim, carrying its world
        // transform (decomposed to TRS). meshObjectNames = the prim's full path
        // — the exact key loadFromFile registers the SceneObject under, so the
        // usd_import SOP's getObject() lookup hits. When the prim's world
        // transform is animated (its own or an ancestor's xformOps carry time
        // samples) we also emit the per-sample TRS so the importer bakes
        // keyframes. Xform-nesting preservation is a follow-up (still flat).
        for (const UsdPrim &prim : stage->Traverse())
        {
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
            decomposeTRS(toGlm(xformable.ComputeLocalToWorldTransform(UsdTimeCode::Default())),
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
                    decomposeTRS(toGlm(xformable.ComputeLocalToWorldTransform(UsdTimeCode(t))),
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
