// Phase-0 EXIT criterion: a multi-file composed stage opens & is consumed by the
// engine via the round-trip, and an `over` in a stronger sublayer composes into the
// result. Authors three files through edit targets —
//
//   base.usda   : def quad mesh + UsdPreviewSurface material, diffuseColor RED
//   over.usda   : an `over` on that shader, diffuseColor GREEN  (stronger layer)
//   shot.usda   : assembly, subLayerPaths = [over, base]  (over wins)
//
// then imports shot.usda through tracey's UsdLoader (the same path the renderer's
// round-trip bridge uses) and checks the imported material albedo == GREEN (the
// override reached the engine Scene that SceneCompiler/the path tracer consume), while
// base.usda alone is still RED (non-destructive).
//
//   usd_compose_smoke [out_dir]   → also leaves shot.usda for pt_backend_compare.

#include "scene/usd_loader.hpp"
#include "scene/scene.hpp"
#include "scene/scene_object.hpp"
#include "scene/scene_instance.hpp"
#include "scene/material_instance.hpp"
#include "scene/actor.hpp"
#include "core/types.hpp"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <cmath>
#include <cstdio>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace tracey;

namespace
{
    const SdfPath kQuad("/root/quad");
    const SdfPath kShader("/root/Looks/Mat/Surface");

    void authorBaseGeometryAndMaterial(const UsdStageRefPtr &stage)
    {
        UsdGeomXform::Define(stage, SdfPath("/root"));
        UsdGeomMesh quad = UsdGeomMesh::Define(stage, kQuad);
        VtArray<GfVec3f> pts = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}};
        quad.GetPointsAttr().Set(pts);
        quad.GetFaceVertexCountsAttr().Set(VtArray<int>{3, 3});
        quad.GetFaceVertexIndicesAttr().Set(VtArray<int>{0, 1, 2, 0, 2, 3});
        quad.GetExtentAttr().Set(VtArray<GfVec3f>{{-1, 0, -1}, {1, 0, 1}});
        VtArray<GfVec3f> nrm = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
        quad.SetNormalsInterpolation(UsdGeomTokens->vertex);
        quad.GetNormalsAttr().Set(nrm);

        UsdGeomScope::Define(stage, SdfPath("/root/Looks"));
        UsdShadeMaterial mat = UsdShadeMaterial::Define(stage, SdfPath("/root/Looks/Mat"));
        UsdShadeShader shader = UsdShadeShader::Define(stage, kShader);
        shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
        shader.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(GfVec3f(0.9f, 0.1f, 0.1f));
        mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
        UsdShadeMaterialBindingAPI::Apply(quad.GetPrim());
        UsdShadeMaterialBindingAPI(quad.GetPrim()).Bind(mat);

        stage->SetDefaultPrim(stage->GetPrimAtPath(SdfPath("/root")));
    }

    float albedoOf(const Scene &scene)
    {
        for (const auto &node : scene.flatten())
        {
            if (!node.actor || !node.actor->visible()) continue;
            for (const auto &inst : node.actor->instances())
            {
                const SceneObject *obj = scene.getObject(inst.objectRef());
                if (!obj || obj->positions().empty()) continue;
                if (auto a = inst.material().albedo()) return a->y; // green channel
            }
        }
        return -1.0f;
    }
}

int main(int argc, char **argv)
{
    if (!UsdLoader::available()) { std::fprintf(stderr, "no USD support\n"); return 2; }
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    const std::string basePath = dir + "/tracey_cmp_base.usda";
    const std::string overPath = dir + "/tracey_cmp_over.usda";
    const std::string shotPath = dir + "/tracey_cmp_shot.usda";

    // ── Author the assembly + two sublayers via edit targets ──
    SdfLayerRefPtr baseLayer = SdfLayer::CreateNew(basePath);
    SdfLayerRefPtr overLayer = SdfLayer::CreateNew(overPath);
    UsdStageRefPtr stage = UsdStage::CreateNew(shotPath);
    if (!baseLayer || !overLayer || !stage) { std::fprintf(stderr, "create failed\n"); return 1; }
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
    stage->GetRootLayer()->SetSubLayerPaths({overPath, basePath}); // over strongest

    stage->SetEditTarget(UsdEditTarget(baseLayer));
    authorBaseGeometryAndMaterial(stage);

    stage->SetEditTarget(UsdEditTarget(overLayer));
    UsdPrim shaderOver = stage->OverridePrim(kShader);
    UsdShadeShader(shaderOver).CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f)
        .Set(GfVec3f(0.1f, 0.9f, 0.1f)); // GREEN wins

    stage->GetRootLayer()->Save();
    baseLayer->Save();
    overLayer->Save();
    std::printf("usd_compose_smoke: authored %s (+ base RED, over GREEN)\n", shotPath.c_str());

    // ── Round-trip through the engine's loader (the renderer's bridge path) ──
    auto composed = UsdLoader::loadFromFile(shotPath);
    auto baseOnly = UsdLoader::loadFromFile(basePath);
    if (!composed || !baseOnly) { std::fprintf(stderr, "FAIL: loader returned null\n"); return 1; }

    const float composedGreen = albedoOf(*composed);
    const float baseGreen = albedoOf(*baseOnly);
    std::printf("  composed albedo.g = %.3g (expect 0.9 — over wins)\n", composedGreen);
    std::printf("  base-only albedo.g = %.3g (expect 0.1 — base untouched)\n", baseGreen);

    const bool ok = std::fabs(composedGreen - 0.9f) < 1e-3f && std::fabs(baseGreen - 0.1f) < 1e-3f;
    std::printf("usd_compose_smoke: %s\n", ok ? "PASS" : "FAIL");
    std::printf("  (render the composed stage with: pt_backend_compare %s)\n", shotPath.c_str());
    return ok ? 0 : 1;
}
