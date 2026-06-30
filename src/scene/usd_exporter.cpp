#include "usd_exporter.hpp"

#ifndef TRACEY_HAS_USD

// ── USD-less stub ────────────────────────────────────────────────────────────
// Keeps the symbol available so non-USD builds link; mirrors UsdLoader's pattern.
namespace tracey
{
    bool UsdExporter::available() { return false; }
    bool UsdExporter::exportToFile(const Scene &, const std::string &, std::string *error)
    {
        if (error) *error = "USD support not compiled in (build with TRACEY_WITH_USD)";
        return false;
    }
} // namespace tracey

#else

#include "scene.hpp"
#include "scene_object.hpp"
#include "scene_instance.hpp"
#include "material_instance.hpp"
#include "actor.hpp"
#include "transform.hpp"
#include "../core/types.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>

#include <glm/gtc/type_ptr.hpp>

#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace tracey
{
    namespace
    {
        // ── Resolved material (engine defaults filled in) ───────────────────
        // Mirrors SceneExporter::ResolvedMaterial / GPUMaterial defaults so a
        // re-import reproduces the same look even when a property is unset.
        struct ResolvedMaterial
        {
            Vec3 albedo{1.0f, 1.0f, 1.0f};
            float opacity = 1.0f;
            float metallic = 0.0f;
            float roughness = 0.5f;
            Vec3 emission{0.0f, 0.0f, 0.0f};
            float ior = 1.5f;
            float emissionStrength = 1.0f;
        };

        ResolvedMaterial resolveMaterial(const MaterialInstance &m)
        {
            ResolvedMaterial r;
            if (auto a = m.albedo()) r.albedo = *a;
            if (auto o = m.getFloat("opacity")) r.opacity = *o;
            if (auto mt = m.metallic()) r.metallic = *mt;
            if (auto rg = m.roughness()) r.roughness = *rg;
            if (auto e = m.emission()) r.emission = *e;
            if (auto i = m.getFloat("ior")) r.ior = *i;
            if (auto es = m.getFloat("emissionStrength")) r.emissionStrength = *es;
            return r;
        }

        std::string materialKey(const ResolvedMaterial &m)
        {
            std::ostringstream os;
            os.precision(6);
            os << m.albedo.x << ',' << m.albedo.y << ',' << m.albedo.z << ',' << m.opacity << '|'
               << m.metallic << ',' << m.roughness << '|'
               << m.emission.x << ',' << m.emission.y << ',' << m.emission.z << '|'
               << m.ior << ',' << m.emissionStrength;
            return os.str();
        }

        // One draw = one instance: a SceneObject + baked world matrix + material.
        struct Draw
        {
            Mat4 world;
            const SceneObject *obj;
            const MaterialInstance *mat;
        };

        std::vector<Draw> collectDraws(const Scene &scene)
        {
            std::vector<Draw> draws;
            for (const auto &node : scene.flatten())
            {
                const Actor *actor = node.actor;
                if (!actor || !actor->visible()) continue;
                for (const auto &inst : actor->instances())
                {
                    const SceneObject *obj = scene.getObject(inst.objectRef());
                    if (!obj || obj->positions().empty()) continue;
                    Mat4 world = node.worldTransform;
                    if (inst.hasLocalTransform())
                        world = world * inst.localTransform()->toMatrix();
                    draws.push_back({world, obj, &inst.material()});
                }
            }
            return draws;
        }

        std::vector<uint32_t> triangleIndices(const SceneObject &obj)
        {
            const auto &idx = obj.indices();
            if (!idx.empty()) return idx;
            std::vector<uint32_t> seq(obj.positions().size());
            for (uint32_t i = 0; i < seq.size(); ++i) seq[i] = i;
            return seq;
        }

        // glm (column-major, column-vector) → GfMatrix4d (row-major, row-vector):
        // feeding value_ptr in storage order into the 16-arg ctor yields the
        // correct USD matrix (the two layout/convention flips cancel).
        GfMatrix4d toGf(const Mat4 &m)
        {
            const float *p = glm::value_ptr(m);
            return GfMatrix4d(p[0], p[1], p[2], p[3],
                              p[4], p[5], p[6], p[7],
                              p[8], p[9], p[10], p[11],
                              p[12], p[13], p[14], p[15]);
        }

        TfToken leafIdentifier(const std::string &name)
        {
            const std::string leaf =
                name.substr(name.find_last_of('/') == std::string::npos ? 0 : name.find_last_of('/') + 1);
            std::string id = TfMakeValidIdentifier(leaf.empty() ? "mesh" : leaf);
            return TfToken(id);
        }

        // Where to author a draw's mesh. Preserve the object's Sdf path when its
        // name already is a valid absolute path (so import→export→re-import keeps
        // identity); otherwise sanitise under /World. Uniquify on collision so an
        // object instanced N times yields N distinct prims.
        SdfPath pathForDraw(const SceneObject &obj, std::unordered_set<std::string> &used)
        {
            const std::string &name = obj.name();
            SdfPath base;
            if (!name.empty() && name[0] == '/' && SdfPath::IsValidPathString(name))
                base = SdfPath(name);
            else
                base = SdfPath("/World").AppendChild(leafIdentifier(name));

            SdfPath p = base;
            int n = 1;
            while (used.count(p.GetString()))
                p = SdfPath(base.GetString() + "_inst" + std::to_string(n++));
            used.insert(p.GetString());
            return p;
        }
    } // namespace

    bool UsdExporter::available() { return true; }

    bool UsdExporter::exportToFile(const Scene &scene, const std::string &path, std::string *error)
    {
        const std::vector<Draw> draws = collectDraws(scene);

        UsdStageRefPtr stage = UsdStage::CreateNew(path);
        if (!stage)
        {
            if (error) *error = "UsdExporter: failed to create stage at " + path;
            return false;
        }
        // Engine works in Y-up; authoring Y-up makes the importer's up-axis fix a
        // no-op so transforms round-trip exactly. Declare a linear scale too (1 unit
        // = 1 m) so the layer is self-describing and usdchecker-clean.
        UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
        UsdGeomSetStageMetersPerUnit(stage, 1.0);

        UsdGeomXform world = UsdGeomXform::Define(stage, SdfPath("/World"));
        stage->SetDefaultPrim(world.GetPrim());
        UsdGeomScope::Define(stage, SdfPath("/World/Looks"));

        // Author one UsdShadeMaterial per unique material; reuse by content key.
        std::unordered_map<std::string, SdfPath> matPaths;
        auto materialFor = [&](const MaterialInstance &mi) -> SdfPath {
            const ResolvedMaterial rm = resolveMaterial(mi);
            const std::string key = materialKey(rm);
            auto it = matPaths.find(key);
            if (it != matPaths.end()) return it->second;

            const SdfPath matPath =
                SdfPath("/World/Looks").AppendChild(TfToken("Mat_" + std::to_string(matPaths.size())));
            UsdShadeMaterial mat = UsdShadeMaterial::Define(stage, matPath);
            UsdShadeShader shader = UsdShadeShader::Define(stage, matPath.AppendChild(TfToken("Surface")));
            shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
            shader.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f)
                .Set(GfVec3f(rm.albedo.x, rm.albedo.y, rm.albedo.z));
            shader.CreateInput(TfToken("metallic"), SdfValueTypeNames->Float).Set(rm.metallic);
            shader.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(rm.roughness);
            shader.CreateInput(TfToken("ior"), SdfValueTypeNames->Float).Set(rm.ior);
            shader.CreateInput(TfToken("opacity"), SdfValueTypeNames->Float).Set(rm.opacity);
            // Fold emissionStrength into emissiveColor — UsdPreviewSurface has no
            // separate strength, and the importer reads emissiveColor directly.
            shader.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f)
                .Set(GfVec3f(rm.emission.x * rm.emissionStrength,
                             rm.emission.y * rm.emissionStrength,
                             rm.emission.z * rm.emissionStrength));
            mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));

            matPaths.emplace(key, matPath);
            return matPath;
        };

        std::unordered_set<std::string> usedPaths;
        for (const Draw &d : draws)
        {
            const SceneObject &obj = *d.obj;
            const SdfPath meshPath = pathForDraw(obj, usedPaths);
            UsdGeomMesh mesh = UsdGeomMesh::Define(stage, meshPath);
            if (!mesh)
            {
                if (error) *error = "UsdExporter: failed to define mesh at " + meshPath.GetString();
                return false;
            }

            // Points (object-local) + extent.
            const auto &positions = obj.positions();
            VtArray<GfVec3f> points(positions.size());
            GfVec3f lo(std::numeric_limits<float>::max());
            GfVec3f hi(std::numeric_limits<float>::lowest());
            for (size_t i = 0; i < positions.size(); ++i)
            {
                points[i] = GfVec3f(positions[i].x, positions[i].y, positions[i].z);
                for (int c = 0; c < 3; ++c)
                {
                    lo[c] = std::min(lo[c], points[i][c]);
                    hi[c] = std::max(hi[c], points[i][c]);
                }
            }
            mesh.GetPointsAttr().Set(points);
            VtArray<GfVec3f> extent(2);
            extent[0] = lo;
            extent[1] = hi;
            mesh.GetExtentAttr().Set(extent);

            // Topology (triangulated).
            const std::vector<uint32_t> tri = triangleIndices(obj);
            VtArray<int> faceVertexIndices(tri.size());
            for (size_t i = 0; i < tri.size(); ++i) faceVertexIndices[i] = int(tri[i]);
            VtArray<int> faceVertexCounts(tri.size() / 3, 3);
            mesh.GetFaceVertexIndicesAttr().Set(faceVertexIndices);
            mesh.GetFaceVertexCountsAttr().Set(faceVertexCounts);
            mesh.SetNormalsInterpolation(UsdGeomTokens->vertex);

            // Normals (per-vertex, 1:1 with points when present).
            if (obj.hasNormals() && obj.normals().size() == positions.size())
            {
                const auto &nrm = obj.normals();
                VtArray<GfVec3f> normals(nrm.size());
                for (size_t i = 0; i < nrm.size(); ++i)
                    normals[i] = GfVec3f(nrm[i].x, nrm[i].y, nrm[i].z);
                mesh.GetNormalsAttr().Set(normals);
            }

            UsdGeomPrimvarsAPI pv(mesh.GetPrim());
            // UVs → `st` primvar (per-vertex).
            if (obj.hasUvs() && obj.uvs().size() == positions.size())
            {
                const auto &uv = obj.uvs();
                VtArray<GfVec2f> st(uv.size());
                for (size_t i = 0; i < uv.size(); ++i) st[i] = GfVec2f(uv[i].x, uv[i].y);
                UsdGeomPrimvar p = pv.CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray,
                                                    UsdGeomTokens->vertex);
                p.Set(st);
            }
            // Vertex colours → displayColor primvar (per-vertex).
            if (obj.hasColors() && obj.colors().size() == positions.size())
            {
                const auto &cd = obj.colors();
                VtArray<GfVec3f> dc(cd.size());
                for (size_t i = 0; i < cd.size(); ++i) dc[i] = GfVec3f(cd[i].x, cd[i].y, cd[i].z);
                UsdGeomPrimvar p = mesh.CreateDisplayColorPrimvar(UsdGeomTokens->vertex);
                p.Set(dc);
            }

            // Baked world transform (single matrix xformOp).
            mesh.MakeMatrixXform().Set(toGf(d.world));

            // Material binding.
            const SdfPath matPath = materialFor(*d.mat);
            UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim());
            UsdShadeMaterialBindingAPI(mesh.GetPrim())
                .Bind(UsdShadeMaterial(stage->GetPrimAtPath(matPath)));
        }

        if (!stage->GetRootLayer()->Save())
        {
            if (error) *error = "UsdExporter: failed to save " + path;
            return false;
        }
        return true;
    }
} // namespace tracey

#endif // TRACEY_HAS_USD
