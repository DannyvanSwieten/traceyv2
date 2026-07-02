// Phase-1a vertical slice: the StageDocument — one asset, one shot, three department
// layers (layout / anim / lighting), each authored through its own edit target, all
// composing into one Scene via the round-trip bridge, persisting and reopening, and —
// the headline — staying NON-DESTRUCTIVELY SEPARATE (each department's opinion lives
// only in its own file).
//
//   asset.usda      : a cube, exported via UsdExporter (dogfoods the writer)
//   layout.usda     : references the asset at /shot/cube
//   anim.usda       : an `over` translating /shot/cube to y=3
//   lighting.usda   : a SphereLight at /shot/keyLight
//   shot.usda       : assembly, subLayers = [lighting, anim, layout]
//
//   stage_document_smoke [out_dir]

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
#include <fstream>
#include <sstream>
#include <string>

using namespace tracey;

namespace
{
    // Write a unit-cube asset layer via UsdExporter (so the slice also dogfoods the
    // writer). Cube centred at origin, half-size 1, blue material.
    bool writeCubeAsset(const std::string &path)
    {
        auto obj = std::make_unique<SceneObject>();
        obj->setName("cube");
        obj->setPositions({{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                           {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}});
        obj->setIndices({0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,
                         3, 7, 6, 3, 6, 2,  1, 2, 6, 1, 6, 5,  0, 4, 7, 0, 7, 3});
        MaterialInstance mat("pbr");
        mat.setAlbedo(Vec3(0.1f, 0.1f, 0.9f));

        Scene scene;
        scene.addObject("cube", std::move(obj));
        Actor *a = scene.createActor();
        a->setName("cube");
        a->addInstance(SceneInstance("cube", mat));

        std::string err;
        if (!UsdExporter::exportToFile(scene, path, &err))
        {
            std::fprintf(stderr, "writeCubeAsset: %s\n", err.c_str());
            return false;
        }
        return true;
    }

    struct Probe
    {
        int meshActors = 0;
        int lights = 0;
        float meshWorldY = 0.0f;
    };
    Probe probe(const Scene &scene)
    {
        Probe p;
        for (const auto &node : scene.flatten())
        {
            const Actor *actor = node.actor;
            if (!actor) continue;
            if (actor->hasLight()) ++p.lights;
            bool hasGeo = false;
            for (const auto &inst : actor->instances())
            {
                const SceneObject *o = scene.getObject(inst.objectRef());
                if (o && !o->positions().empty()) hasGeo = true;
            }
            if (hasGeo)
            {
                ++p.meshActors;
                p.meshWorldY = node.worldTransform[3].y;
            }
        }
        return p;
    }

    // First light of `type` in the composed scene, or nullptr.
    const Light *findLight(const Scene &scene, LightType type)
    {
        for (const auto *a : scene.actors())
            if (a && a->hasLight() && a->light()->type == type) return a->light();
        return nullptr;
    }

    std::string readFile(const std::string &path)
    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    bool has(const std::string &s, const std::string &sub) { return s.find(sub) != std::string::npos; }
}

int main(int argc, char **argv)
{
    if (!StageDocument::available()) { std::fprintf(stderr, "no USD support\n"); return 2; }
    const std::string dir = (argc > 1 ? std::string(argv[1]) : std::string("/tmp")) + "/";
    const std::string assetPath = dir + "sd_asset.usda";
    const std::string shotPath = dir + "sd_shot.usda";

    if (!writeCubeAsset(assetPath)) return 1;

    // ── Author the shot through department edit targets ──
    auto doc = StageDocument::createShot(shotPath, {"lighting", "anim", "layout"});
    if (!doc) { std::fprintf(stderr, "FAIL: createShot\n"); return 1; }

    bool authored = true;
    authored &= doc->setActiveDepartment("layout");
    authored &= doc->referenceAsset("/shot/cube", assetPath);
    authored &= doc->setActiveDepartment("anim");
    authored &= doc->setPrimTransform("/shot/cube", Vec3(0, 3, 0), Vec3(0), Vec3(1, 1, 1));
    authored &= doc->setActiveDepartment("lighting");
    // Every editor light type through defineLight, with distinctive params — the
    // reopened scene must give them back type-correct AND param-correct (the old
    // author-everything-as-SphereLight bug degraded Sun/Area/Dome on recompose).
    Light key;                      // Point
    key.type = LightType::Point;
    key.intensity = 1000.0f;
    key.radius = 0.25f;
    authored &= doc->defineLight("/shot/keyLight", key,
                                 glm::translate(glm::mat4(1.0f), glm::vec3(0, 10, 0)));
    Light sun;                      // Distant
    sun.type = LightType::Distant;
    sun.intensity = 5.0f;
    sun.color = Vec3(1.0f, 0.9f, 0.8f);
    authored &= doc->defineLight("/shot/sun", sun, glm::mat4(1.0f));
    Light panel;                    // Area
    panel.type = LightType::Area;
    panel.intensity = 40.0f;
    panel.size = Vec2(2.0f, 0.5f);
    authored &= doc->defineLight("/shot/panel", panel,
                                 glm::translate(glm::mat4(1.0f), glm::vec3(0, 5, 0)));
    Light env;                      // Dome, custom gradient
    env.type = LightType::Dome;
    env.intensity = 2.0f;
    env.skyColor = Vec3(0.1f, 0.2f, 0.9f);
    env.groundColor = Vec3(0.3f, 0.2f, 0.1f);
    authored &= doc->defineLight("/shot/env", env, glm::mat4(1.0f));
    if (!authored) { std::fprintf(stderr, "FAIL: authoring returned false\n"); return 1; }

    // ── Compose (before save) ──
    auto live = doc->toScene();
    if (!live) { std::fprintf(stderr, "FAIL: toScene (live)\n"); return 1; }
    const Probe pl = probe(*live);
    std::printf("  live     : %d mesh actor(s), %d light(s), mesh world.y = %.3g\n",
                pl.meshActors, pl.lights, pl.meshWorldY);

    if (!doc->save()) { std::fprintf(stderr, "FAIL: save\n"); return 1; }

    // ── Reopen from disk and recompose (persistence) ──
    auto reopened = StageDocument::openShot(shotPath);
    if (!reopened) { std::fprintf(stderr, "FAIL: openShot\n"); return 1; }
    auto rs = reopened->toScene();
    if (!rs) { std::fprintf(stderr, "FAIL: toScene (reopened)\n"); return 1; }
    const Probe pr = probe(*rs);
    std::printf("  reopened : %d mesh actor(s), %d light(s), mesh world.y = %.3g\n",
                pr.meshActors, pr.lights, pr.meshWorldY);
    std::printf("  departments: ");
    for (const auto &d : reopened->departments()) std::printf("%s ", d.c_str());
    std::printf("(strongest first)\n");

    // ── Non-destructive separation: each opinion lives only in its own layer ──
    const std::string layout = readFile(dir + "layout.usda");
    const std::string anim = readFile(dir + "anim.usda");
    const std::string lighting = readFile(dir + "lighting.usda");

    bool ok = true;
    auto check = [&](bool cond, const char *msg) { if (!cond) { std::fprintf(stderr, "FAIL: %s\n", msg); ok = false; } };

    // Composition correct, both before and after a save/reopen.
    check(pl.meshActors == 1 && pr.meshActors == 1, "expected exactly 1 mesh actor");
    check(pl.lights == 4 && pr.lights == 4, "expected exactly 4 lights");
    check(std::fabs(pl.meshWorldY - 3.0f) < 1e-3f, "live mesh not at anim y=3");
    check(std::fabs(pr.meshWorldY - 3.0f) < 1e-3f, "reopened mesh not at anim y=3 (persistence)");

    // Light round-trip: every type comes back type-correct with its params.
    {
        const Light *pt = findLight(*rs, LightType::Point);
        check(pt && std::fabs(pt->intensity - 1000.0f) < 1e-2f && std::fabs(pt->radius - 0.25f) < 1e-4f,
              "Point light params lost in round-trip");
        const Light *dst = findLight(*rs, LightType::Distant);
        check(dst && std::fabs(dst->intensity - 5.0f) < 1e-4f && std::fabs(dst->color.z - 0.8f) < 1e-4f,
              "Distant light lost/degraded in round-trip");
        const Light *ar = findLight(*rs, LightType::Area);
        check(ar && std::fabs(ar->size.x - 2.0f) < 1e-4f && std::fabs(ar->size.y - 0.5f) < 1e-4f,
              "Area light size lost in round-trip");
        const Light *dm = findLight(*rs, LightType::Dome);
        check(dm && std::fabs(dm->intensity - 2.0f) < 1e-4f &&
                  std::fabs(dm->skyColor.z - 0.9f) < 1e-4f && std::fabs(dm->groundColor.x - 0.3f) < 1e-4f,
              "Dome light gradient (tracey:* attrs) lost in round-trip");
    }

    // The reference lives only in layout (USD nests scopes, so check the asset ref +
    // the cube prim rather than a flattened "/shot/cube" path).
    check(has(layout, "references") && has(layout, "sd_asset") && has(layout, "def \"cube\""),
          "layout missing the reference");
    check(!has(layout, "SphereLight") && !has(layout, "(0, 3, 0)"), "layout contaminated");
    // The anim transform lives only in anim.
    check(has(anim, "xformOp:translate") && has(anim, "(0, 3, 0)"), "anim missing the transform");
    check(!has(anim, "SphereLight") && !has(anim, "references"), "anim contaminated");
    // The light lives only in lighting.
    check(has(lighting, "SphereLight") && has(lighting, "keyLight"), "lighting missing the light");
    check(!has(lighting, "references") && !has(lighting, "(0, 3, 0)"), "lighting contaminated");

    std::printf("stage_document_smoke: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
