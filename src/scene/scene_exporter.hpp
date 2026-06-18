#pragma once
#include <string>

namespace tracey
{
    class Scene;

    // Serialises the live scene's visible, cooked geometry to a standard
    // interchange file so a traceyv2 scene can be handed off to other DCCs
    // (Blender / Unreal / etc.). This is the counterpart to GltfLoader: it
    // walks Scene::flatten() × each Actor's SceneInstances, dedupes shared
    // SceneObjects into one mesh, and writes one node per instance with the
    // instance's baked world transform and per-instance material.
    //
    // glTF/GLB is the primary target (re-importable through GltfLoader,
    // preserves PBR factors + transmission/ior/emission via KHR extensions);
    // OBJ is a baked world-space fallback for maximum tool reach. Texture
    // images are not embedded yet — material factors only (see the .cpp).
    class SceneExporter
    {
    public:
        enum class Format
        {
            GltfJson, // .gltf (JSON, buffers embedded as base64 — self-contained)
            Glb,      // .glb  (binary, buffers in the BIN chunk)
            Obj,      // .obj  (+ sidecar .mtl, world-space baked triangles)
        };

        // Exports `scene` (at its current cooked frame) to `path`. Returns true
        // on success; on failure returns false and, when `error` is non-null,
        // fills it with a human-readable message.
        static bool exportToFile(const Scene &scene, const std::string &path,
                                 Format format, std::string *error = nullptr);
    };
} // namespace tracey
