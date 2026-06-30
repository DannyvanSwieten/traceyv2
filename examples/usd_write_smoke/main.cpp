// Phase-0 smoke: prove the department-layer mechanism the whole USD pipeline rests
// on — author opinions into SEPARATE sublayer files via edit targets, compose them
// with a strength order, and confirm the stronger layer wins while neither file
// mutates the other (non-destructive). This models a shot.usd that sublayers a strong
// department layer (e.g. lighting / an override) above a weak one (the asset/base).
//
//   usd_write_smoke [out_dir]
//
// PASS criteria:
//   * the composed stage reads the STRONG layer's opinion (strength order honoured),
//   * the base layer file, opened alone, still holds ONLY its own opinion (the strong
//     layer never wrote into it — non-destructive separation).

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>

#include <cstdio>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    const std::string basePath = dir + "/tracey_ws_base.usda";     // weak  : asset / geometry
    const std::string strongPath = dir + "/tracey_ws_strong.usda"; // strong: an override dept
    const std::string rootPath = dir + "/tracey_ws_shot.usda";     // assembly: sublayers both

    // ── Author the assembly + its two department sublayers ──
    SdfLayerRefPtr baseLayer = SdfLayer::CreateNew(basePath);
    SdfLayerRefPtr strongLayer = SdfLayer::CreateNew(strongPath);
    if (!baseLayer || !strongLayer)
    {
        std::fprintf(stderr, "usd_write_smoke: failed to create sublayers\n");
        return 1;
    }

    UsdStageRefPtr stage = UsdStage::CreateNew(rootPath);
    if (!stage)
    {
        std::fprintf(stderr, "usd_write_smoke: failed to create root stage %s\n", rootPath.c_str());
        return 1;
    }

    // Strength order: the FRONT of subLayerPaths is the strongest opinion.
    stage->GetRootLayer()->SetSubLayerPaths({strongPath, basePath});

    // Weak/base (asset) layer: DEFINE the geometry, radius 2.
    stage->SetEditTarget(UsdEditTarget(baseLayer));
    UsdGeomXform::Define(stage, SdfPath("/root"));
    UsdGeomSphere ball = UsdGeomSphere::Define(stage, SdfPath("/root/ball"));
    ball.GetRadiusAttr().Set(2.0);
    stage->SetDefaultPrim(stage->GetPrimAtPath(SdfPath("/root")));

    // Strong (override) layer: an `over` on the same prim that wins the radius.
    stage->SetEditTarget(UsdEditTarget(strongLayer));
    UsdPrim ballOver = stage->OverridePrim(SdfPath("/root/ball"));
    UsdGeomSphere(ballOver).GetRadiusAttr().Set(5.0);

    stage->GetRootLayer()->Save();
    baseLayer->Save();
    strongLayer->Save();
    std::printf("usd_write_smoke: authored %s (+ base + strong sublayers)\n", rootPath.c_str());

    // ── Reopen the composed stage — the strong layer must win ──
    UsdStageRefPtr composed = UsdStage::Open(rootPath);
    if (!composed)
    {
        std::fprintf(stderr, "usd_write_smoke: failed to reopen composed stage\n");
        return 1;
    }
    double composedRadius = 0.0;
    UsdGeomSphere(composed->GetPrimAtPath(SdfPath("/root/ball"))).GetRadiusAttr().Get(&composedRadius);

    // ── Open the base layer ALONE — it must still hold only its own opinion ──
    UsdStageRefPtr baseOnly = UsdStage::Open(baseLayer);
    double baseRadius = 0.0;
    UsdGeomSphere(baseOnly->GetPrimAtPath(SdfPath("/root/ball"))).GetRadiusAttr().Get(&baseRadius);

    std::printf("  composed radius  = %g (expect 5 — strong layer wins)\n", composedRadius);
    std::printf("  base-only radius = %g (expect 2 — base file untouched)\n", baseRadius);

    const bool ok = (composedRadius == 5.0) && (baseRadius == 2.0);
    std::printf("usd_write_smoke: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
