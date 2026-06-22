#pragma once

#include "scene.hpp"

#include <memory>
#include <string>

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

        // True when the build links OpenUSD (TRACEY_HAS_USD).
        static bool available();
    };
}
