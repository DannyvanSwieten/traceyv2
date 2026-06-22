// U1.4 smoke: validates the USD import contract the editor's subnet importer
// relies on — that UsdLoader::peekHierarchy and loadFromFileCached stay in
// LOCKSTEP. For every mesh prim peek reports, the prim-path key it emits in
// `meshObjectNames` MUST resolve via the loaded Scene's getObject(); otherwise
// the usd_import SOP cooks empty geometry and the import silently produces no
// actor (the failure mode glTF import learned the hard way).
//
// Also confirms the bound UsdPreviewSurface material is recoverable the way
// apply_emitted's pullUsdMaterial does (walk actors → SceneInstance whose
// objectRef == the prim path → its MaterialInstance).
//
//   usd_import_smoke <scene.usd[a|c|z]>

#include "scene/usd_loader.hpp"
#include "scene/scene.hpp"
#include "scene/scene_object.hpp"
#include "scene/actor.hpp"
#include "scene/scene_instance.hpp"
#include "scene/material_instance.hpp"

#include <cstdio>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: usd_import_smoke <scene.usd>\n");
        return 2;
    }
    const std::string path = argv[1];

    if (!tracey::UsdLoader::available())
    {
        std::fprintf(stderr, "usd_import_smoke: build has no USD support\n");
        return 2;
    }

    // ── Peek ──
    auto roots = tracey::UsdLoader::peekHierarchy(path);
    std::printf("peek: %zu mesh node(s)\n", roots.size());
    for (const auto &n : roots)
    {
        std::printf("  node '%s'  T=(%.3g,%.3g,%.3g) R=(%.3g,%.3g,%.3g) S=(%.3g,%.3g,%.3g)  meshes=%zu\n",
                    n.name.c_str(),
                    n.translate.x, n.translate.y, n.translate.z,
                    n.rotateEulerDeg.x, n.rotateEulerDeg.y, n.rotateEulerDeg.z,
                    n.scale.x, n.scale.y, n.scale.z,
                    n.meshObjectNames.size());
    }

    // ── Load (cached — the SOP cook + apply_emitted share this) ──
    auto scene = tracey::UsdLoader::loadFromFileCached(path);
    if (!scene)
    {
        std::fprintf(stderr, "FAIL: loadFromFileCached returned null\n");
        return 1;
    }
    // Second call must hit the cache → identical pointer.
    auto scene2 = tracey::UsdLoader::loadFromFileCached(path);
    const bool cacheHit = (scene.get() == scene2.get());

    int ok = 1;
    if (roots.empty()) { std::fprintf(stderr, "FAIL: peek found no mesh prims\n"); ok = 0; }
    if (!cacheHit)     { std::fprintf(stderr, "FAIL: loadFromFileCached did not memoize\n"); ok = 0; }

    // ── Lockstep: every peeked key resolves, and carries geometry ──
    int materialsFound = 0;
    for (const auto &n : roots)
    {
        for (const auto &key : n.meshObjectNames)
        {
            const auto *obj = scene->getObject(key);
            if (!obj)
            {
                std::fprintf(stderr, "FAIL: peek key '%s' does not resolve via getObject\n", key.c_str());
                ok = 0;
                continue;
            }
            if (obj->positions().empty())
            {
                std::fprintf(stderr, "FAIL: object '%s' has no positions\n", key.c_str());
                ok = 0;
            }
            std::printf("  resolved '%s' → %zu verts, %zu indices\n",
                        key.c_str(), obj->positions().size(), obj->indices().size());

            // Material recovery (mirrors apply_emitted::pullUsdMaterial).
            for (const auto &a : scene->actors())
            {
                if (!a) continue;
                for (const auto &inst : a->instances())
                {
                    if (inst.objectRef() == key)
                    {
                        const auto &mat = inst.material();
                        if (auto albedo = mat.albedo())
                        {
                            std::printf("    material albedo = (%.3g, %.3g, %.3g)\n",
                                        albedo->x, albedo->y, albedo->z);
                            ++materialsFound;
                        }
                    }
                }
            }
        }
    }

    std::printf("materials recovered: %d\n", materialsFound);
    std::printf("usd_import_smoke: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
