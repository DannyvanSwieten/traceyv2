// Phase-0 golden round-trip: prove UsdExporter writes a USD layer that re-imports
// to the same scene. Imports a USD asset → exports via UsdExporter → re-imports, and
// compares draw count, vertex/triangle totals, the world-space bounding box, and
// aggregate material albedo. Geometry + baked transforms + materials must survive.
//
//   usd_roundtrip_smoke <asset.usd> [out.usda]
//
// (Names may change across the trip — meshes sanitise under /World when not already
// a valid Sdf path — so the comparison is identity-agnostic: it checks the rendered
// result, not the prim keys.)

#include "scene/usd_loader.hpp"
#include "scene/usd_exporter.hpp"
#include "scene/scene.hpp"
#include "scene/scene_object.hpp"
#include "scene/scene_instance.hpp"
#include "scene/material_instance.hpp"
#include "scene/actor.hpp"
#include "scene/transform.hpp"
#include "core/types.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace tracey;

namespace
{
    struct Summary
    {
        int draws = 0;
        size_t verts = 0;
        size_t tris = 0;
        Vec3 lo{std::numeric_limits<float>::max()};
        Vec3 hi{std::numeric_limits<float>::lowest()};
        Vec3 albedoSum{0.0f};
    };

    Summary summarize(const Scene &scene)
    {
        Summary s;
        for (const auto &node : scene.flatten())
        {
            const Actor *actor = node.actor;
            if (!actor || !actor->visible()) continue;
            for (const auto &inst : actor->instances())
            {
                const SceneObject *obj = scene.getObject(inst.objectRef());
                if (!obj || obj->positions().empty()) continue;
                Mat4 world = node.worldTransform;
                if (inst.hasLocalTransform()) world = world * inst.localTransform()->toMatrix();

                ++s.draws;
                s.verts += obj->positions().size();
                s.tris += obj->indices().empty() ? obj->positions().size() / 3
                                                  : obj->indices().size() / 3;
                for (const Vec3 &p : obj->positions())
                {
                    const glm::vec4 w = world * glm::vec4(p.x, p.y, p.z, 1.0f);
                    for (int c = 0; c < 3; ++c)
                    {
                        s.lo[c] = std::min(s.lo[c], w[c]);
                        s.hi[c] = std::max(s.hi[c], w[c]);
                    }
                }
                // Compare the RESOLVED albedo (default white when unset) — the
                // exporter writes resolved values, and unset renders as the default,
                // so an identity-faithful trip turns nullopt into an explicit (1,1,1).
                Vec3 alb{1.0f, 1.0f, 1.0f};
                if (auto a = inst.material().albedo()) alb = *a;
                s.albedoSum += alb;
            }
        }
        return s;
    }

    bool close(float a, float b, float relEps = 1e-2f, float absEps = 1e-3f)
    {
        return std::fabs(a - b) <= absEps + relEps * std::max(std::fabs(a), std::fabs(b));
    }
    bool close3(const Vec3 &a, const Vec3 &b)
    {
        return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
    }
}

int main(int argc, char **argv)
{
    if (!UsdLoader::available() || !UsdExporter::available())
    {
        std::fprintf(stderr, "usd_roundtrip_smoke: build has no USD support\n");
        return 2;
    }
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: usd_roundtrip_smoke <asset.usd> [out.usda]\n");
        return 2;
    }
    const std::string in = argv[1];
    const std::string out = argc > 2 ? argv[2] : "/tmp/tracey_roundtrip.usda";

    // ── Import → export → re-import ──
    auto a = UsdLoader::loadFromFile(in);
    if (!a) { std::fprintf(stderr, "FAIL: could not import %s\n", in.c_str()); return 1; }

    std::string err;
    if (!UsdExporter::exportToFile(*a, out, &err))
    {
        std::fprintf(stderr, "FAIL: export: %s\n", err.c_str());
        return 1;
    }
    auto b = UsdLoader::loadFromFile(out);
    if (!b) { std::fprintf(stderr, "FAIL: could not re-import %s\n", out.c_str()); return 1; }

    const Summary sa = summarize(*a), sb = summarize(*b);
    std::printf("  original : %d draws, %zu verts, %zu tris, bbox[(%.3g,%.3g,%.3g)-(%.3g,%.3g,%.3g)]\n",
                sa.draws, sa.verts, sa.tris, sa.lo.x, sa.lo.y, sa.lo.z, sa.hi.x, sa.hi.y, sa.hi.z);
    std::printf("  roundtrip: %d draws, %zu verts, %zu tris, bbox[(%.3g,%.3g,%.3g)-(%.3g,%.3g,%.3g)]\n",
                sb.draws, sb.verts, sb.tris, sb.lo.x, sb.lo.y, sb.lo.z, sb.hi.x, sb.hi.y, sb.hi.z);

    bool ok = true;
    if (sa.draws != sb.draws) { std::fprintf(stderr, "FAIL: draw count %d != %d\n", sa.draws, sb.draws); ok = false; }
    if (sa.verts != sb.verts) { std::fprintf(stderr, "FAIL: vert count %zu != %zu\n", sa.verts, sb.verts); ok = false; }
    if (sa.tris  != sb.tris)  { std::fprintf(stderr, "FAIL: tri count %zu != %zu\n", sa.tris, sb.tris); ok = false; }
    if (!close3(sa.lo, sb.lo) || !close3(sa.hi, sb.hi)) { std::fprintf(stderr, "FAIL: bbox drift\n"); ok = false; }
    if (!close3(sa.albedoSum, sb.albedoSum))
    {
        std::fprintf(stderr, "FAIL: albedo sum (%.3g,%.3g,%.3g) != (%.3g,%.3g,%.3g)\n",
                     sa.albedoSum.x, sa.albedoSum.y, sa.albedoSum.z,
                     sb.albedoSum.x, sb.albedoSum.y, sb.albedoSum.z);
        ok = false;
    }

    std::printf("usd_roundtrip_smoke: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
