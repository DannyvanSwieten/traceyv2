#pragma once

#include "scene.hpp"
#include "../core/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tracey
{
    // Imports an OpenUSD stage (.usd / .usda / .usdc / .usdz) into our Scene,
    // mirroring GltfLoader. The header is deliberately USD-free (no pxr types in
    // the API) so anything can include it; the implementation lives in the
    // separate `tracey_usd` library, which is the only thing that links USD.
    //
    // Built behind TRACEY_WITH_USD (deps/usd via bootstrap). When the build has
    // no USD support, loadFromFile() returns nullptr and available() is false —
    // the core build never hard-depends on this heavy dependency.
    //
    // First slice (U1.1): UsdGeomMesh geometry (triangulated, per-vertex
    // normals + `st`), each mesh's world transform baked onto its actor, and
    // bound UsdPreviewSurface materials → MaterialInstance. Cameras, UsdLux
    // lights, PointInstancer instancing and time-sampled animation are
    // follow-ups (U1.2).
    class UsdLoader
    {
    public:
        // Open `path` and convert it to a Scene. Returns nullptr on failure or
        // when USD support isn't compiled in.
        static std::unique_ptr<Scene> loadFromFile(const std::string &path);

        // Process-wide memo of parsed stages, keyed by path. Mirrors
        // GltfLoader::loadFromFileCached: the SOP graph spawns one usd_import
        // per mesh prim and the editor's apply_emitted re-resolves materials
        // per actor, so all those callers share one parsed Scene instead of
        // re-opening the stage every time. Shared ownership; the cache keeps a
        // reference until invalidated or process exit. Thread-safe. Returns
        // nullptr on failure / no USD support.
        static std::shared_ptr<const Scene> loadFromFileCached(const std::string &path);

        // Drop any cached entry for `path` (e.g. the file was re-saved).
        static void invalidateCache(const std::string &path);

        // ── Hierarchy peek ───────────────────────────────────────────────
        // Lightweight description of the stage used by the editor's subnet
        // importer (mirrors GltfLoader::HierarchyNode). First slice is FLAT:
        // one node per UsdGeomMesh prim, carrying the prim's *world* transform
        // as TRS (rotation pre-converted to ZYX intrinsic euler-degrees so it
        // drops straight into a SOP `rotate_euler_deg` param). `meshObjectNames`
        // holds the single SceneObject key the usd_import SOP looks up — the
        // prim's full Sdf path, matching loadFromFile's object naming so peek
        // and load stay in lockstep. Geometry travels in the prim's local
        // space, so the subnet's world TRS positions it correctly. Nested Xform
        // hierarchy preservation is a follow-up.
        struct HierarchyNode
        {
            std::string name;
            Vec3 translate{0.0f};
            Vec3 rotateEulerDeg{0.0f};
            Vec3 scale{1.0f};
            std::vector<std::string> meshObjectNames;
            std::vector<HierarchyNode> children;
        };

        // Reads geometry structurally enough to compute per-prim world
        // transforms, but skips heavy buffer decode where possible. Returns one
        // root node per mesh prim. Returns an empty vector on failure / when USD
        // support isn't compiled in.
        static std::vector<HierarchyNode> peekHierarchy(const std::string &path);

        // True when the build links OpenUSD (TRACEY_HAS_USD).
        static bool available();
    };
}
