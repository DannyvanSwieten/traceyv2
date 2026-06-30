#include "stage_document.hpp"

#ifndef TRACEY_HAS_USD

namespace tracey
{
    struct StageDocument::Impl {};
    StageDocument::StageDocument() = default;
    StageDocument::~StageDocument() = default;
    StageDocument::StageDocument(StageDocument &&) noexcept = default;
    StageDocument &StageDocument::operator=(StageDocument &&) noexcept = default;
    bool StageDocument::available() { return false; }
    std::unique_ptr<StageDocument> StageDocument::createShot(const std::string &, const std::vector<std::string> &) { return nullptr; }
    std::unique_ptr<StageDocument> StageDocument::openShot(const std::string &) { return nullptr; }
    static const std::vector<std::string> kNoDepts;
    static const std::string kNoActive;
    const std::vector<std::string> &StageDocument::departments() const { return kNoDepts; }
    const std::string &StageDocument::activeDepartment() const { return kNoActive; }
    bool StageDocument::setActiveDepartment(const std::string &) { return false; }
    std::string StageDocument::layerPath(const std::string &) const { return {}; }
    std::string StageDocument::shotName() const { return {}; }
    std::vector<double> StageDocument::primKeyTimes(const std::string &) const { return {}; }
    bool StageDocument::referenceAsset(const std::string &, const std::string &) { return false; }
    bool StageDocument::removePrim(const std::string &) { return false; }
    bool StageDocument::setPrimTransform(const std::string &, const Vec3 &, const Vec3 &, const Vec3 &) { return false; }
    bool StageDocument::defineSphereLight(const std::string &, const Vec3 &, float, const Vec3 &) { return false; }
    bool StageDocument::defineDomeLight(const std::string &, float, const Vec3 &) { return false; }
    std::unique_ptr<Scene> StageDocument::toScene() const { return nullptr; }
    std::unique_ptr<Scene> StageDocument::toSceneAtTime(double) const { return nullptr; }
    bool StageDocument::setPrimMatrixAtTime(const std::string &, double, const Mat4 &) { return false; }
    bool StageDocument::setPrimTRSAtTime(const std::string &, double, const Vec3 &, const Vec3 &, const Vec3 &) { return false; }
    bool StageDocument::save() const { return false; }
} // namespace tracey

#else

#include "scene.hpp"
#include "usd_internal.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/sphereLight.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace tracey
{
    struct StageDocument::Impl
    {
        UsdStageRefPtr stage;
        std::vector<std::string> deptOrder; // strongest first
        std::unordered_map<std::string, SdfLayerRefPtr> layers;
        std::string active;
        std::string shotPath; // the shot.usd path (for shotName())

        SdfLayerRefPtr activeLayer() const
        {
            auto it = layers.find(active);
            return it == layers.end() ? SdfLayerRefPtr() : it->second;
        }
        bool routeToActive() const
        {
            SdfLayerRefPtr l = activeLayer();
            if (!l) return false;
            stage->SetEditTarget(UsdEditTarget(l));
            return true;
        }
        // Express `assetPath` RELATIVE to the layer that will hold the opinion (the
        // active layer), so references/sublayers are portable — no absolute paths
        // baked into the project's USD. Falls back to the input if a relative path
        // can't be formed (e.g. different volumes).
        std::string relativeToActiveLayer(const std::string &assetPath) const
        {
            SdfLayerRefPtr l = activeLayer();
            if (!l) return assetPath;
            const std::string layerReal = l->GetRealPath();
            if (layerReal.empty()) return assetPath;
            std::error_code ec;
            std::filesystem::path rel = std::filesystem::relative(
                std::filesystem::path(assetPath),
                std::filesystem::path(layerReal).parent_path(), ec);
            if (ec || rel.empty()) return assetPath;
            return rel.generic_string();
        }
    };

    namespace
    {
        std::string dirOf(const std::string &path)
        {
            const auto slash = path.find_last_of('/');
            return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
        }
        std::string stemOf(const std::string &path)
        {
            const auto slash = path.find_last_of('/');
            std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
            const auto dot = file.find_last_of('.');
            return dot == std::string::npos ? file : file.substr(0, dot);
        }
    } // namespace

    StageDocument::StageDocument() : m(std::make_unique<Impl>()) {}
    StageDocument::~StageDocument() = default;
    StageDocument::StageDocument(StageDocument &&) noexcept = default;
    StageDocument &StageDocument::operator=(StageDocument &&) noexcept = default;

    bool StageDocument::available() { return true; }

    std::unique_ptr<StageDocument> StageDocument::createShot(
        const std::string &shotPath, const std::vector<std::string> &departments)
    {
        UsdStageRefPtr stage = UsdStage::CreateNew(shotPath);
        if (!stage)
        {
            std::cerr << "[stagedoc] failed to create shot " << shotPath << std::endl;
            return nullptr;
        }
        UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
        UsdGeomSetStageMetersPerUnit(stage, 1.0);

        // Shot skeleton in the root (assembly) layer: a /shot Xform as the default
        // prim. Department contributions compose under it.
        UsdGeomXform::Define(stage, SdfPath("/shot"));
        stage->SetDefaultPrim(stage->GetPrimAtPath(SdfPath("/shot")));

        auto doc = std::unique_ptr<StageDocument>(new StageDocument());
        doc->m->stage = stage;
        doc->m->shotPath = shotPath;

        const std::string dir = dirOf(shotPath);
        std::vector<std::string> subLayers; // front = strongest, matches `departments`
        for (const std::string &dept : departments)
        {
            const std::string layerPath = dir + dept + ".usda";
            SdfLayerRefPtr layer = SdfLayer::CreateNew(layerPath);
            if (!layer)
            {
                std::cerr << "[stagedoc] failed to create department layer " << layerPath << std::endl;
                return nullptr;
            }
            doc->m->deptOrder.push_back(dept);
            doc->m->layers.emplace(dept, layer);
            // Relative sublayer path (same dir as shot.usda) so the shot is portable.
            subLayers.push_back(dept + ".usda");
        }
        stage->GetRootLayer()->SetSubLayerPaths(subLayers);

        if (!departments.empty()) doc->setActiveDepartment(departments.back()); // weakest by default
        return doc;
    }

    std::unique_ptr<StageDocument> StageDocument::openShot(const std::string &shotPath)
    {
        UsdStageRefPtr stage = UsdStage::Open(shotPath);
        if (!stage)
        {
            std::cerr << "[stagedoc] failed to open shot " << shotPath << std::endl;
            return nullptr;
        }
        auto doc = std::unique_ptr<StageDocument>(new StageDocument());
        doc->m->stage = stage;
        doc->m->shotPath = shotPath;

        const std::string dir = dirOf(shotPath);
        for (const std::string &sub : stage->GetRootLayer()->GetSubLayerPaths())
        {
            // Resolve relative to the root layer's directory (matches how it composed).
            const std::string resolved = (!sub.empty() && sub[0] == '/') ? sub : dir + sub;
            SdfLayerRefPtr layer = SdfLayer::FindOrOpen(resolved);
            if (!layer) continue;
            const std::string dept = stemOf(sub);
            doc->m->deptOrder.push_back(dept);
            doc->m->layers.emplace(dept, layer);
        }
        if (!doc->m->deptOrder.empty()) doc->setActiveDepartment(doc->m->deptOrder.back());
        return doc;
    }

    const std::vector<std::string> &StageDocument::departments() const { return m->deptOrder; }
    const std::string &StageDocument::activeDepartment() const { return m->active; }

    std::string StageDocument::shotName() const
    {
        // file "<seq>/<shot>/shot.usd" → "seq/shot"; a named file → its stem.
        const std::string stem = stemOf(m->shotPath);
        if (stem != "shot") return stem;
        const std::string dir = dirOf(m->shotPath); // ".../<seq>/<shot>/"
        // strip trailing slash, take last component, and the one above it
        std::string d = dir;
        if (!d.empty() && d.back() == '/') d.pop_back();
        const std::string shot = d.substr(d.find_last_of('/') == std::string::npos ? 0 : d.find_last_of('/') + 1);
        const std::string up = dirOf(d);
        std::string u = up;
        if (!u.empty() && u.back() == '/') u.pop_back();
        const std::string seq = u.substr(u.find_last_of('/') == std::string::npos ? 0 : u.find_last_of('/') + 1);
        if (!seq.empty() && seq != "shots") return seq + "/" + shot;
        return shot.empty() ? std::string("shot") : shot;
    }

    bool StageDocument::setActiveDepartment(const std::string &dept)
    {
        if (!m->layers.count(dept)) return false;
        m->active = dept;
        return m->routeToActive();
    }

    std::string StageDocument::layerPath(const std::string &dept) const
    {
        auto it = m->layers.find(dept);
        return it == m->layers.end() ? std::string() : it->second->GetRealPath();
    }

    bool StageDocument::referenceAsset(const std::string &primPath, const std::string &assetPath)
    {
        if (!m->routeToActive()) return false;
        UsdPrim p = m->stage->DefinePrim(SdfPath(primPath));
        if (!p) return false;
        return p.GetReferences().AddReference(m->relativeToActiveLayer(assetPath));
    }

    std::string StageDocument::referenceAssetAuto(const std::string &assetPath,
                                                  const std::string &baseName)
    {
        if (!m->routeToActive()) return {};
        // Sanitise to a valid prim name (alnum/_; must start with a letter or _).
        std::string clean;
        for (char c : baseName)
        {
            const bool an = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_';
            clean += an ? c : '_';
        }
        if (clean.empty() ||
            !((clean[0] >= 'A' && clean[0] <= 'Z') || (clean[0] >= 'a' && clean[0] <= 'z') || clean[0] == '_'))
            clean = "asset_" + clean;

        const std::string base = "/shot/" + clean;
        std::string path = base;
        int n = 1;
        while (m->stage->GetPrimAtPath(SdfPath(path)).IsValid())
            path = base + "_" + std::to_string(n++);

        UsdPrim p = m->stage->DefinePrim(SdfPath(path));
        if (!p) return {};
        if (!p.GetReferences().AddReference(m->relativeToActiveLayer(assetPath))) return {};
        return path;
    }

    bool StageDocument::removePrim(const std::string &primPath)
    {
        if (primPath.empty()) return false;
        const SdfPath p(primPath);
        if (p.IsEmpty()) return false;
        // The actor maps to a mesh prim deep inside a referenced instance; remove the
        // INSTANCE root (the direct child of the shot's default prim), not the leaf mesh.
        const SdfPath shotRoot = m->stage->GetDefaultPrim()
                                     ? m->stage->GetDefaultPrim().GetPath()
                                     : SdfPath("/shot");
        SdfPath root = p;
        while (!root.IsEmpty() && root.GetParentPath() != shotRoot &&
               root.GetParentPath() != SdfPath::AbsoluteRootPath())
            root = root.GetParentPath();
        if (root.IsEmpty()) root = p;

        // Remove the prim spec from EVERY department layer that defines or overrides it,
        // so the instance is gone regardless of which department authored it (the layout
        // reference + any anim/lighting overs) and nothing dangles. Restore the active
        // edit target afterwards.
        bool removed = false;
        for (const auto &kv : m->layers)
        {
            m->stage->SetEditTarget(UsdEditTarget(kv.second));
            if (m->stage->RemovePrim(root)) removed = true;
        }
        m->routeToActive();
        return removed;
    }

    bool StageDocument::setPrimTransform(const std::string &primPath, const Vec3 &t,
                                         const Vec3 &r, const Vec3 &s)
    {
        if (!m->routeToActive()) return false;
        UsdPrim p = m->stage->OverridePrim(SdfPath(primPath));
        if (!p) return false;
        UsdGeomXformable x(p);
        x.ClearXformOpOrder(); // author a clean TRS order in the active layer
        x.AddTranslateOp().Set(GfVec3d(t.x, t.y, t.z));         // double precision default
        x.AddRotateXYZOp().Set(GfVec3f(r.x, r.y, r.z));         // float precision default
        x.AddScaleOp().Set(GfVec3f(s.x, s.y, s.z));             // float precision default
        return true;
    }

    bool StageDocument::setPrimMatrix(const std::string &primPath, const Mat4 &mtx)
    {
        if (!m->routeToActive()) return false;
        UsdPrim p = m->stage->OverridePrim(SdfPath(primPath));
        if (!p) return false;
        UsdGeomXformable x(p);
        x.ClearXformOpOrder();
        // glm (col-major, col-vector) → GfMatrix4d: feeding value_ptr storage order
        // into the 16-arg ctor yields the correct USD matrix (see usd_exporter).
        const float *pp = glm::value_ptr(mtx);
        GfMatrix4d gf(pp[0], pp[1], pp[2], pp[3], pp[4], pp[5], pp[6], pp[7],
                      pp[8], pp[9], pp[10], pp[11], pp[12], pp[13], pp[14], pp[15]);
        x.MakeMatrixXform().Set(gf);
        return true;
    }

    bool StageDocument::defineSphereLight(const std::string &primPath, const Vec3 &position,
                                          float intensity, const Vec3 &color)
    {
        if (!m->routeToActive()) return false;
        UsdLuxSphereLight light = UsdLuxSphereLight::Define(m->stage, SdfPath(primPath));
        if (!light) return false;
        light.CreateIntensityAttr(VtValue(intensity));
        light.CreateColorAttr(VtValue(GfVec3f(color.x, color.y, color.z)));
        UsdGeomXformable(light.GetPrim()).AddTranslateOp().Set(GfVec3d(position.x, position.y, position.z));
        return true;
    }

    bool StageDocument::defineDomeLight(const std::string &primPath, float intensity, const Vec3 &color)
    {
        if (!m->routeToActive()) return false;
        UsdLuxDomeLight light = UsdLuxDomeLight::Define(m->stage, SdfPath(primPath));
        if (!light) return false;
        light.CreateIntensityAttr(VtValue(intensity));
        light.CreateColorAttr(VtValue(GfVec3f(color.x, color.y, color.z)));
        return true; // dome is transform-independent — no placement op
    }

    std::unique_ptr<Scene> StageDocument::toScene() const
    {
        return convertStageToScene(m->stage, "<stage document>");
    }

    std::unique_ptr<Scene> StageDocument::toSceneAtTime(double timeCode) const
    {
        return convertStageToScene(m->stage, "<stage document>", UsdTimeCode(timeCode));
    }

    std::vector<double> StageDocument::primKeyTimes(const std::string &primPath) const
    {
        std::vector<double> out;
        if (primPath.empty()) return out;
        UsdPrim p = m->stage->GetPrimAtPath(SdfPath(primPath));
        if (!p) return out;
        UsdGeomXformable x(p);
        bool reset = false;
        for (const UsdGeomXformOp &op : x.GetOrderedXformOps(&reset))
        {
            std::vector<double> ts;
            if (op.GetAttr().GetTimeSamples(&ts)) out.insert(out.end(), ts.begin(), ts.end());
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    bool StageDocument::setPrimMatrixAtTime(const std::string &primPath, double timeCode, const Mat4 &mtx)
    {
        if (!m->routeToActive()) return false;
        UsdPrim p = m->stage->OverridePrim(SdfPath(primPath));
        if (!p) return false;
        UsdGeomXformable x(p);
        // Reuse an existing matrix op or create one once — don't clear the op order
        // each call, so accumulated time samples (keys) persist on the same attr.
        UsdGeomXformOp op;
        bool resets = false;
        for (const UsdGeomXformOp &o : x.GetOrderedXformOps(&resets))
            if (o.GetOpType() == UsdGeomXformOp::TypeTransform) { op = o; break; }
        if (!op) op = x.MakeMatrixXform();
        if (!op) return false;
        const float *pp = glm::value_ptr(mtx);
        GfMatrix4d gf(pp[0], pp[1], pp[2], pp[3], pp[4], pp[5], pp[6], pp[7],
                      pp[8], pp[9], pp[10], pp[11], pp[12], pp[13], pp[14], pp[15]);
        op.Set(gf, UsdTimeCode(timeCode));
        return true;
    }

    bool StageDocument::setPrimTRSAtTime(const std::string &primPath, double timeCode,
                                         const Vec3 &t, const Vec3 &rDeg, const Vec3 &s)
    {
        if (!m->routeToActive()) return false;
        UsdPrim p = m->stage->OverridePrim(SdfPath(primPath));
        if (!p) return false;
        UsdGeomXformable x(p);
        // Reuse existing translate / rotateXYZ / scale ops so accumulated time samples
        // (keys) persist; on the first key (or if a different op set — e.g. a baked
        // matrix — is present) author a clean TRS order in the active layer.
        UsdGeomXformOp tOp, rOp, sOp;
        bool resets = false;
        for (const UsdGeomXformOp &o : x.GetOrderedXformOps(&resets))
        {
            const auto ty = o.GetOpType();
            if (ty == UsdGeomXformOp::TypeTranslate) tOp = o;
            else if (ty == UsdGeomXformOp::TypeRotateXYZ) rOp = o;
            else if (ty == UsdGeomXformOp::TypeScale) sOp = o;
        }
        if (!tOp || !rOp || !sOp)
        {
            x.ClearXformOpOrder();
            tOp = x.AddTranslateOp();
            rOp = x.AddRotateXYZOp();
            sOp = x.AddScaleOp();
        }
        if (!tOp || !rOp || !sOp) return false;
        tOp.Set(GfVec3d(t.x, t.y, t.z), UsdTimeCode(timeCode));
        rOp.Set(GfVec3f(rDeg.x, rDeg.y, rDeg.z), UsdTimeCode(timeCode));
        sOp.Set(GfVec3f(s.x, s.y, s.z), UsdTimeCode(timeCode));
        return true;
    }

    bool StageDocument::save() const
    {
        bool ok = m->stage->GetRootLayer()->Save();
        for (const auto &kv : m->layers)
            ok = kv.second->Save() && ok;
        return ok;
    }
} // namespace tracey

#endif // TRACEY_HAS_USD
