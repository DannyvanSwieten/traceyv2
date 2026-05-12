#pragma once
#include "scene.hpp"
#include "../core/types.hpp"
#include <string>
#include <memory>
#include <vector>

namespace tracey
{
    class GltfLoader
    {
    public:
        // Load a GLTF/GLB file and convert it to our Scene format
        static std::unique_ptr<Scene> loadFromFile(const std::string &path);

        // Options for loading
        struct LoadOptions
        {
            bool loadMaterials = true;
            bool loadNormals = true;
            bool loadTexCoords = true;
            float scaleFactor = 1.0f;
        };

        static std::unique_ptr<Scene> loadFromFile(const std::string &path, const LoadOptions &options);

        // Process-wide memo of parsed scenes, keyed by path. Repeated callers
        // for the same path (the SOP graph spawns one gltf_import per primitive,
        // and the editor's apply_emitted re-resolves materials per actor) share
        // one parsed Scene + decoded texture set instead of re-reading the file
        // for every importer. Caller gets shared ownership; the cache also
        // keeps a reference until process exit (no eviction policy yet — fine
        // while only a handful of unique glTFs are open at once). Thread-safe.
        static std::shared_ptr<const Scene> loadFromFileCached(const std::string &path);

        // Drop any cached entry for `path` — useful when the editor knows the
        // file has been re-saved and the next cook should re-read it.
        static void invalidateCache(const std::string &path);

        // ── Hierarchy peek ───────────────────────────────────────────────
        // A lightweight description of a glTF node used by the editor's
        // recursive subnet importer: it walks this tree and synthesises one
        // subnet per node + a gltf_import/object_output chain inside each
        // mesh-bearing leaf.
        //
        // Rotation is exposed as euler-degrees (ZYX intrinsic, matching the
        // convention used elsewhere — transform_sop, the subnet's cook
        // emit, set_actor_rotation_euler) so the importer can populate SOP
        // `rotate_euler_deg` params directly with no conversion at the
        // call site.
        //
        // `meshObjectNames` lists every SceneObject the node's mesh expands
        // into — one entry per primitive of the referenced mesh (multi-prim
        // meshes are common when artists pack sub-materials into a single
        // mesh; emitting them all here lets the importer wire up one
        // gltf_import per primitive instead of silently dropping all but
        // the first). Empty for transform-only nodes (node.mesh < 0).
        struct HierarchyNode
        {
            std::string name;
            Vec3 translate{0.0f};
            Vec3 rotateEulerDeg{0.0f};
            Vec3 scale{1.0f};
            std::vector<std::string> meshObjectNames;
            std::vector<HierarchyNode> children;
        };

        // Reads only the structural metadata — no buffers, accessors, or
        // images — so even large files peek quickly. Returns the top-level
        // nodes of the default scene (or all root nodes if no scenes are
        // declared). Throws on parse failure; returns an empty vector for
        // a valid but empty file.
        static std::vector<HierarchyNode> peekHierarchy(const std::string &path);
    };
}
