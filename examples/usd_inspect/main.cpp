// usd_inspect — dump the material/texture structure of a USD stage, so we can
// see WHY a real asset (e.g. Pixar Kitchen Set) does or doesn't texture under
// our importer. Pure USD inspection (no tracey types): for each mesh prim it
// reports geometry primvars (st / displayColor / normals + interpolation) and,
// for the bound UsdPreviewSurface, every input — constant value vs connection,
// the connected node's shader id, and for UsdUVTexture the file path + whether
// it actually exists on disk (and in a format stb_image can read).
//
//   usd_inspect <scene.usd[a|c|z]> [maxMeshes]

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/sdf/assetPath.h>

#include <cstdio>
#include <filesystem>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

static std::string lowerExt(const std::string &p)
{
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = p.substr(dot + 1);
    for (auto &c : e) c = static_cast<char>(tolower(c));
    return e;
}

// Follow one input: print whether it's a constant or a connection, and if it
// resolves to a UsdUVTexture, the file path + existence + readability.
static void dumpInput(const UsdShadeShader &surface, const char *name)
{
    UsdShadeInput in = surface.GetInput(TfToken(name));
    if (!in) { std::printf("      %-14s : (absent)\n", name); return; }

    const UsdShadeSourceInfoVector sources = in.GetConnectedSources();
    if (sources.empty() || !sources[0].source)
    {
        // Constant value (just note it's authored; type varies).
        std::printf("      %-14s : constant\n", name);
        return;
    }

    UsdShadeShader node(sources[0].source.GetPrim());
    TfToken id;
    if (node) node.GetIdAttr().Get(&id);
    std::printf("      %-14s -> %s (%s)", name,
                node ? node.GetPrim().GetName().GetString().c_str() : "?",
                id.GetString().c_str());

    if (node && id == TfToken("UsdUVTexture"))
    {
        SdfAssetPath asset;
        if (UsdShadeInput f = node.GetInput(TfToken("file")); f && f.Get(&asset))
        {
            std::string resolved = asset.GetResolvedPath();
            const std::string authored = asset.GetAssetPath();
            const std::string &path = !resolved.empty() ? resolved : authored;
            const bool exists = !path.empty() && std::filesystem::exists(path);
            const std::string ext = lowerExt(path);
            const bool stbReadable = (ext == "png" || ext == "jpg" || ext == "jpeg" ||
                                      ext == "tga" || ext == "bmp" || ext == "gif" ||
                                      ext == "hdr" || ext == "psd");
            std::printf("\n                       file: \"%s\"\n", authored.c_str());
            std::printf("                       resolved: \"%s\" [%s, ext=%s, %s]",
                        resolved.c_str(),
                        exists ? "EXISTS" : "MISSING",
                        ext.empty() ? "?" : ext.c_str(),
                        stbReadable ? "stb-readable" : "NOT stb-readable");
        }
        // What feeds its `st`? (a primvar reader, or a UsdTransform2d chain)
        if (UsdShadeInput stIn = node.GetInput(TfToken("st")))
        {
            const auto stSrc = stIn.GetConnectedSources();
            if (!stSrc.empty() && stSrc[0].source)
            {
                UsdShadeShader stNode(stSrc[0].source.GetPrim());
                TfToken stId;
                if (stNode) stNode.GetIdAttr().Get(&stId);
                std::printf("\n                       st <- %s", stId.GetString().c_str());
            }
        }
    }
    std::printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: usd_inspect <scene.usd> [maxMeshes]\n"); return 2; }
    const std::string path = argv[1];
    const int maxMeshes = argc > 2 ? atoi(argv[2]) : 20;

    UsdStageRefPtr stage = UsdStage::Open(path);
    if (!stage) { std::fprintf(stderr, "failed to open %s\n", path.c_str()); return 1; }

    std::printf("stage: %s\n", path.c_str());
    std::printf("upAxis: %s\n", UsdGeomGetStageUpAxis(stage).GetString().c_str());

    int meshCount = 0, shown = 0, withMaterial = 0, withTexture = 0;
    for (const UsdPrim &prim : stage->Traverse())
    {
        UsdGeomMesh mesh(prim);
        if (!mesh) continue;
        ++meshCount;

        UsdShadeMaterial mat = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
        UsdShadeShader surface = mat ? mat.ComputeSurfaceSource() : UsdShadeShader();
        if (mat) ++withMaterial;

        if (shown >= maxMeshes) continue;
        ++shown;

        std::printf("\nmesh '%s'\n", prim.GetPath().GetString().c_str());

        // Geometry primvars.
        UsdGeomPrimvarsAPI pv(prim);
        for (const char *nm : {"st", "displayColor"})
        {
            UsdGeomPrimvar p = pv.GetPrimvar(TfToken(nm));
            if (!p) { std::printf("    primvar %-12s (absent)\n", nm); continue; }
            std::printf("    primvar %-12s interp=%s", nm, p.GetInterpolation().GetString().c_str());
            if (std::string(nm) == "displayColor")
            {
                VtArray<GfVec3f> c;
                if (p.ComputeFlattened(&c) && !c.empty())
                    std::printf("  value[0]=(%.3g, %.3g, %.3g)  count=%zu",
                                c[0][0], c[0][1], c[0][2], c.size());
            }
            std::printf("\n");
        }
        {
            TfToken ni = mesh.GetNormalsInterpolation();
            VtArray<GfVec3f> n;
            std::printf("    normals      %s interp=%s\n",
                        mesh.GetNormalsAttr().Get(&n) && !n.empty() ? "present" : "absent",
                        ni.GetString().c_str());
        }

        if (!mat) { std::printf("    material: (none bound)\n"); continue; }
        if (!surface) { std::printf("    material '%s' but no surface source\n",
                                    mat.GetPath().GetString().c_str()); continue; }
        TfToken sid;
        surface.GetIdAttr().Get(&sid);
        std::printf("    material '%s' surface=%s\n",
                    mat.GetPath().GetString().c_str(), sid.GetString().c_str());

        bool anyTex = false;
        for (const char *in : {"diffuseColor", "metallic", "roughness", "normal",
                               "emissiveColor", "opacity"})
        {
            UsdShadeInput i = surface.GetInput(TfToken(in));
            if (i && !i.GetConnectedSources().empty()) anyTex = true;
            dumpInput(surface, in);
        }
        if (anyTex) ++withTexture;
    }

    std::printf("\n--- summary: %d mesh(es), %d with bound material, %d with a connected input ---\n",
                meshCount, withMaterial, withTexture);
    return 0;
}
