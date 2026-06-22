// U0 smoke test: prove we link OpenUSD and can author + round-trip a stage.
// Creates a .usda with an Xform + a Sphere, saves it, reopens it, traverses
// the prims, and reads an attribute back. No imaging/Hydra — USD core only.
//
//   usd_smoke [out.usda]

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/sdf/path.h>

#include <cstdio>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char **argv)
{
    const std::string path = argc > 1 ? argv[1] : "/tmp/tracey_usd_smoke.usda";

    // ── Author ──
    UsdStageRefPtr stage = UsdStage::CreateNew(path);
    if (!stage)
    {
        std::fprintf(stderr, "usd_smoke: failed to create stage at %s\n", path.c_str());
        return 1;
    }
    UsdGeomXform root = UsdGeomXform::Define(stage, SdfPath("/root"));
    UsdGeomSphere ball = UsdGeomSphere::Define(stage, SdfPath("/root/ball"));
    ball.GetRadiusAttr().Set(2.0);
    stage->SetDefaultPrim(root.GetPrim());
    stage->GetRootLayer()->Save();
    std::printf("usd_smoke: authored %s\n", path.c_str());

    // ── Round-trip ──
    UsdStageRefPtr reopened = UsdStage::Open(path);
    if (!reopened)
    {
        std::fprintf(stderr, "usd_smoke: failed to reopen %s\n", path.c_str());
        return 1;
    }
    int prims = 0;
    for (const UsdPrim &prim : reopened->Traverse())
    {
        std::printf("  %s  (%s)\n", prim.GetPath().GetString().c_str(),
                    prim.GetTypeName().GetString().c_str());
        ++prims;
    }
    double radius = 0.0;
    UsdGeomSphere(reopened->GetPrimAtPath(SdfPath("/root/ball"))).GetRadiusAttr().Get(&radius);
    std::printf("usd_smoke: reopened %d prims; /root/ball radius = %g\n", prims, radius);

    const bool ok = prims >= 2 && radius == 2.0;
    std::printf("usd_smoke: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
