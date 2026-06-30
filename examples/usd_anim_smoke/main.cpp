// Verifies shot-mode animation end-to-end, headlessly: author transform KEYFRAMES
// (USD time-sampled matrix xformOps) into the anim layer at frames 0 and 24, then
// sample the composed scene at frames 0 / 12 / 24 and confirm the actor's world X
// interpolates 0 → 5 → 10. Exercises StageDocument::setPrimMatrixAtTime + toSceneAtTime
// + convertStageToScene(timeCode).
//
//   usd_anim_smoke [out_dir]

#include "scene/stage_document.hpp"
#include "scene/usd_exporter.hpp"
#include "scene/scene.hpp"
#include "scene/scene_object.hpp"
#include "scene/scene_instance.hpp"
#include "scene/material_instance.hpp"
#include "scene/actor.hpp"
#include "core/types.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

using namespace tracey;

namespace
{
    bool writeCubeAsset(const std::string& path)
    {
        auto obj = std::make_unique<SceneObject>();
        obj->setName("cube");
        obj->setPositions({{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                           {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}});
        obj->setIndices({0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                         3, 7, 6, 3, 6, 2, 1, 2, 6, 1, 6, 5, 0, 4, 7, 0, 7, 3});
        Scene scene;
        scene.addObject("cube", std::move(obj));
        Actor* a = scene.createActor();
        a->setName("cube");
        a->addInstance(SceneInstance("cube", MaterialInstance("pbr")));
        std::string err;
        return UsdExporter::exportToFile(scene, path, &err);
    }

    // World X of the (single) mesh actor in a composed scene.
    float meshWorldX(const Scene& scene)
    {
        for (const auto& node : scene.flatten())
        {
            if (!node.actor) continue;
            for (const auto& inst : node.actor->instances())
            {
                const SceneObject* o = scene.getObject(inst.objectRef());
                if (o && !o->positions().empty()) return node.worldTransform[3].x;
            }
        }
        return std::numeric_limits<float>::quiet_NaN();
    }

    // Prim path of the (single) mesh actor — the path a->name() gives, the exact
    // thing the editor keys (the MESH under the reference, not the instance root).
    std::string meshActorName(const Scene& scene)
    {
        for (const auto& node : scene.flatten())
            if (node.actor)
                for (const auto& inst : node.actor->instances())
                {
                    const SceneObject* o = scene.getObject(inst.objectRef());
                    if (o && !o->positions().empty()) return node.actor->name();
                }
        return {};
    }

    // World X-basis (column 0) of the mesh actor — reveals rotation.
    glm::vec3 meshWorldCol0(const Scene& scene)
    {
        for (const auto& node : scene.flatten())
            if (node.actor)
                for (const auto& inst : node.actor->instances())
                {
                    const SceneObject* o = scene.getObject(inst.objectRef());
                    if (o && !o->positions().empty()) return glm::vec3(node.worldTransform[0]);
                }
        return glm::vec3(std::numeric_limits<float>::quiet_NaN());
    }

    bool near(float a, float b) { return std::fabs(a - b) < 0.05f; }
}

int main(int argc, char** argv)
{
    if (!StageDocument::available()) { std::fprintf(stderr, "no USD support\n"); return 2; }
    const std::string dir = (argc > 1 ? std::string(argv[1]) : std::string("/tmp")) + "/";
    const std::string assetPath = dir + "anim_asset.usda";
    const std::string shotPath = dir + "anim_shot.usda";

    if (!writeCubeAsset(assetPath)) { std::fprintf(stderr, "FAIL: writeCubeAsset\n"); return 1; }

    auto doc = StageDocument::createShot(shotPath, {"anim", "layout"});
    if (!doc) { std::fprintf(stderr, "FAIL: createShot\n"); return 1; }

    bool ok = true;
    ok &= doc->setActiveDepartment("layout");
    ok &= doc->referenceAsset("/shot/box", assetPath);
    ok &= doc->setActiveDepartment("anim");
    // Two keyframes: x=0 at frame 0, x=10 at frame 24.
    ok &= doc->setPrimMatrixAtTime("/shot/box", 0.0, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0)));
    ok &= doc->setPrimMatrixAtTime("/shot/box", 24.0, glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)));
    if (!ok) { std::fprintf(stderr, "FAIL: authoring returned false\n"); return 1; }

    const float x0 = meshWorldX(*doc->toSceneAtTime(0.0));
    const float x12 = meshWorldX(*doc->toSceneAtTime(12.0));
    const float x24 = meshWorldX(*doc->toSceneAtTime(24.0));
    std::printf("  world.x @ frame 0 = %.3f (expect 0)\n", x0);
    std::printf("  world.x @ frame 12 = %.3f (expect 5)\n", x12);
    std::printf("  world.x @ frame 24 = %.3f (expect 10)\n", x24);

    const bool pass = near(x0, 0.0f) && near(x12, 5.0f) && near(x24, 10.0f);

    // --- ROTATION round-trip, exactly as the editor does it: a fresh shot, key the
    //     MESH prim (a->name(), the path found in the composed scene), 90° about Y at
    //     frame 24. At 24 the world X-basis should map (1,0,0) → (0,0,-1). ---
    std::filesystem::create_directories(dir + "rot");
    const std::string rotShot = dir + "rot/rot_shot.usda";
    auto rdoc = StageDocument::createShot(rotShot, {"anim", "layout"});
    bool rok = rdoc != nullptr;
    if (rok)
    {
        rok &= rdoc->setActiveDepartment("layout");
        rok &= rdoc->referenceAsset("/shot/box", assetPath);
    }
    const std::string meshPrim = rok ? meshActorName(*rdoc->toScene()) : std::string{};
    std::printf("  rotation: mesh prim = '%s'\n", meshPrim.c_str());
    rok &= !meshPrim.empty();
    if (rok)
    {
        rdoc->setActiveDepartment("anim");
        rok &= rdoc->setPrimMatrixAtTime(meshPrim, 0.0, glm::mat4(1.0f));
        rok &= rdoc->setPrimMatrixAtTime(
            meshPrim, 24.0, glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0)));
    }
    glm::vec3 c0 = rok ? meshWorldCol0(*rdoc->toSceneAtTime(0.0)) : glm::vec3(0);
    glm::vec3 c24 = rok ? meshWorldCol0(*rdoc->toSceneAtTime(24.0)) : glm::vec3(0);
    std::printf("  rotation: worldCol0 @0  = (%.2f, %.2f, %.2f)  expect (1, 0, 0)\n", c0.x, c0.y, c0.z);
    std::printf("  rotation: worldCol0 @24 = (%.2f, %.2f, %.2f)  expect (0, 0, -1)\n", c24.x, c24.y, c24.z);
    const bool rotPass = rok && near(c0.x, 1.0f) && near(c24.x, 0.0f) && near(c24.z, -1.0f);
    std::printf("  rotation: %s\n", rotPass ? "PASS" : "FAIL");

    // --- The -180° → 180° case (the bug the owner hit): keyed as EULER rotateXYZ it
    //     must sweep a full 360°, so the MIDPOINT passes through 0 (identity, col0 =
    //     (1,0,0)). A baked-matrix key would hold the identical -180==180 orientation
    //     (col0 = (-1,0,0)) → "it doesn't rotate". This is what setPrimTRSAtTime fixes. ---
    std::filesystem::create_directories(dir + "spin");
    auto sdoc = StageDocument::createShot(dir + "spin/spin_shot.usda", {"anim", "layout"});
    bool sok = sdoc != nullptr;
    if (sok) { sok &= sdoc->setActiveDepartment("layout"); sok &= sdoc->referenceAsset("/shot/box", assetPath); }
    const std::string spinPrim = sok ? meshActorName(*sdoc->toScene()) : std::string{};
    sok &= !spinPrim.empty();
    if (sok)
    {
        sdoc->setActiveDepartment("anim");
        sok &= sdoc->setPrimTRSAtTime(spinPrim, 0.0, {0, 0, 0}, {0, -180, 0}, {1, 1, 1});
        sok &= sdoc->setPrimTRSAtTime(spinPrim, 24.0, {0, 0, 0}, {0, 180, 0}, {1, 1, 1});
    }
    glm::vec3 sc0 = sok ? meshWorldCol0(*sdoc->toSceneAtTime(0.0)) : glm::vec3(0);
    glm::vec3 sc12 = sok ? meshWorldCol0(*sdoc->toSceneAtTime(12.0)) : glm::vec3(0);
    std::printf("  spin -180..180: col0 @0  = (%.2f, %.2f, %.2f)  expect (-1, 0, 0)\n", sc0.x, sc0.y, sc0.z);
    std::printf("  spin -180..180: col0 @12 = (%.2f, %.2f, %.2f)  expect (1, 0, 0) [swept through 0]\n", sc12.x, sc12.y, sc12.z);
    const bool spinPass = sok && near(sc0.x, -1.0f) && near(sc12.x, 1.0f);
    std::printf("  spin -180..180: %s\n", spinPass ? "PASS" : "FAIL");

    // Persist + reopen so the time samples survive a round-trip.
    doc->save();
    auto re = StageDocument::openShot(shotPath);
    const float r12 = re ? meshWorldX(*re->toSceneAtTime(12.0)) : -999.0f;
    std::printf("  reopened world.x @ 12 = %.3f (expect 5)\n", r12);

    const bool ok2 = pass && rotPass && spinPass && near(r12, 5.0f);
    std::printf("usd_anim_smoke: %s\n", ok2 ? "PASS" : "FAIL");
    return ok2 ? 0 : 1;
}
