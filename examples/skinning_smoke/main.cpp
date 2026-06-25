// Skinning smoke test (character-animation Phase 1).
//
// Loads a rigged + animated glTF (CesiumMan), then checks the invariants the
// skinning pipeline rests on:
//   1. The skinned mesh imports with JOINTS_0/WEIGHTS_0 (hasSkin()).
//   2. A Skeleton parsed: joints, inverse-bind matrices, ≥1 animation clip.
//   3. Bind-pose round-trip: skinning matrices evaluated at the bind pose are
//      all ~identity (worldBind · inverseBind == I), so a bind-pose skin
//      reproduces the original vertices exactly. This is the core correctness
//      check — if inverse-bind or the hierarchy walk is wrong, it fails here.
//   4. Animation actually moves joints: matrices mid-clip differ from identity.
//
// Usage: skinning_smoke [path/to/rigged.glb]
//   defaults to examples/scenes/CesiumMan.glb relative to CWD.

#include "../../src/scene/gltf_loader.hpp"
#include "../../src/scene/scene.hpp"
#include "../../src/scene/scene_object.hpp"
#include "../../src/scene/skeleton.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace tracey;

namespace
{
    int g_failures = 0;

    void check(bool cond, const char *what)
    {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
        if (!cond)
            ++g_failures;
    }

    float maxAbsDiff(const Mat4 &a, const Mat4 &b)
    {
        float worst = 0.0f;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                worst = std::max(worst, std::fabs(a[c][r] - b[c][r]));
        return worst;
    }
}

int main(int argc, char **argv)
{
    const std::string path = (argc > 1) ? argv[1] : "examples/scenes/CesiumMan.glb";
    std::printf("Skinning smoke: %s\n", path.c_str());

    std::shared_ptr<const Scene> scene;
    try
    {
        scene = GltfLoader::loadFromFileCached(path);
    }
    catch (const std::exception &e)
    {
        std::printf("FAIL: load threw: %s\n", e.what());
        return 1;
    }
    if (!scene)
    {
        std::printf("FAIL: null scene\n");
        return 1;
    }

    // Find the first skinned object.
    const SceneObject *skinned = nullptr;
    for (const auto &[name, obj] : scene->objects())
        if (obj && obj->hasSkin())
        {
            skinned = obj.get();
            std::printf("Skinned object: '%s' (%zu verts)\n", name.c_str(), obj->vertexCount());
            break;
        }

    check(skinned != nullptr, "found a skinned mesh (JOINTS_0/WEIGHTS_0 + skeleton)");
    if (!skinned)
        return 1;

    const auto &skel = *skinned->skeleton();
    std::printf("Skeleton: %zu joints, %zu inverse-bind, %zu clips, %zu nodes\n",
                skel.joints.size(), skel.inverseBind.size(), skel.clips.size(), skel.nodes.size());

    check(!skel.joints.empty(), "skeleton has joints");
    check(skel.inverseBind.size() == skel.joints.size(), "inverse-bind count matches joints");
    check(skel.animated(), "at least one animation clip parsed");
    check(skinned->jointWeights().size() == skinned->vertexCount(), "weights are 1:1 with positions");

    // (3) Bind-pose consistency. Per the glTF skinning spec, at the bind pose
    // every joint's skinning matrix equals the mesh node's world transform
    // (inverseBind[j] = inverse(jointWorldBind[j]) · meshNodeWorld), i.e. a
    // single rigid placement shared by all joints. So a bind-pose skin is a
    // rigid transform of the mesh — it preserves shape exactly. A wrong
    // inverse-bind or hierarchy walk would make the per-joint matrices disagree.
    // (We render skinned actors at identity, so baking worldOf·inverseBind
    // yields correct world-space vertices — the meshNodeWorld term is exactly
    // the placement, not an error.)
    {
        const auto skin = skel.skinningMatrices(0.0, /*clipIndex=*/999999);
        float spread = 0.0f;
        for (const auto &m : skin)
            spread = std::max(spread, maxAbsDiff(m, skin[0]));
        std::printf("Bind-pose per-joint matrix spread = %.6f\n", spread);
        check(spread < 1e-3f, "bind-pose skinning matrices agree (rigid placement)");
    }

    // (3b) Mesh-local placement: with the loader's bind shift
    // (inverse(meshNodeBindWorld)) applied, every bind-pose matrix becomes the
    // identity — so the deformed rest mesh equals the original in the mesh
    // node's local space, and the engine's actor transform places it correctly.
    {
        const Mat4 shift = skinned->skinBindShift();
        const auto bind = skel.skinningMatrices(0.0, /*clipIndex=*/999999);
        Mat4 I(1.0f);
        float worst = 0.0f;
        for (const auto &m : bind)
            worst = std::max(worst, maxAbsDiff(shift * m, I));
        std::printf("Mesh-local bind worst |shift*M - I| = %.6f\n", worst);
        check(worst < 1e-3f, "bind shift yields identity in mesh-local space");
    }

    // (4) Animation moves joints: mid-clip matrices must differ from the bind pose.
    if (skel.animated())
    {
        const float dur = skel.clips[0].duration;
        const auto bind = skel.skinningMatrices(0.0, /*clipIndex=*/999999);
        const auto mid = skel.skinningMatrices(dur * 0.5, 0);
        float worst = 0.0f;
        for (size_t k = 0; k < mid.size() && k < bind.size(); ++k)
            worst = std::max(worst, maxAbsDiff(mid[k], bind[k]));
        std::printf("Clip '%s' dur=%.3fs; mid-clip vs bind spread = %.6f\n",
                    skel.clips[0].name.c_str(), dur, worst);
        check(worst > 1e-3f, "animation deforms the skeleton away from bind");
    }

    // (5) Joint world matrices + bone connectivity (skeleton overlay data).
    {
        const auto jw = skel.jointWorldMatrices(0.0, /*clipIndex=*/999999);
        const auto parents = skel.jointParents();
        check(jw.size() == skel.joints.size(), "jointWorldMatrices sized per joint");
        check(parents.size() == skel.joints.size(), "jointParents sized per joint");
        int roots = 0, bones = 0;
        for (int p : parents)
            (p < 0) ? ++roots : ++bones;
        std::printf("Overlay: %d root joint(s), %d bone segment(s)\n", roots, bones);
        check(roots >= 1, "at least one root joint");
        check(bones >= 1, "at least one bone segment");
    }

    // (6) FK pose override: a local-rotation delta on a joint's node changes the
    // posed skeleton (its own orientation + its descendants' positions).
    {
        Skeleton::PoseOverrides ov(skel.nodes.size(), glm::quat(1, 0, 0, 0));
        const int jn = skel.joints[0];
        if (jn >= 0 && static_cast<size_t>(jn) < ov.size())
            ov[jn] = glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 0, 1));
        const auto base = skel.jointWorldMatrices(0.0, /*clip=*/999999, nullptr);
        const auto posed = skel.jointWorldMatrices(0.0, /*clip=*/999999, &ov);
        float worst = 0.0f;
        for (size_t k = 0; k < base.size() && k < posed.size(); ++k)
            worst = std::max(worst, maxAbsDiff(base[k], posed[k]));
        std::printf("FK override world-matrix change = %.6f\n", worst);
        check(worst > 1e-3f, "FK pose override moves the posed skeleton");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "OK" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
