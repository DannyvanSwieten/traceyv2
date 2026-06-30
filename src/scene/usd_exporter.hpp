#pragma once

#include <string>

namespace tracey
{
    class Scene;

    // Authors the live scene's visible, cooked geometry + materials into a single
    // OpenUSD layer (.usd / .usda / .usdc, by extension). The USD counterpart to
    // SceneExporter: walks Scene::flatten() × each Actor's SceneInstances, dedupes
    // shared SceneObjects, writes one UsdGeomMesh per draw (geometry in object-local
    // space, the instance's baked world transform on a single matrix xformOp), and
    // binds a UsdPreviewSurface per unique material.
    //
    // Prim paths preserve a SceneObject's Sdf path when its name already is one (USD
    // import keys objects by full prim path — usd_loader), so a USD import → export →
    // re-import round-trips by identity; procedurally-named objects are sanitised
    // under /World. Stage up-axis is authored Y (the engine's working space), so the
    // importer's up-axis correction is a no-op and transforms round-trip.
    //
    // This is the round-trip writer underpinning the department-layer authoring path
    // (each department will author its own layer this way, via an edit target).
    //
    // The header is USD-free; the implementation lives in the `tracey_usd` library
    // (the only thing that links USD). When built without TRACEY_WITH_USD,
    // exportToFile() returns false and available() is false.
    class UsdExporter
    {
    public:
        // True when this build links OpenUSD (export is functional).
        static bool available();

        // Export `scene` (at its current cooked frame) to `path`. Returns true on
        // success; on failure returns false and, when `error` is non-null, fills it
        // with a human-readable message.
        static bool exportToFile(const Scene &scene, const std::string &path,
                                 std::string *error = nullptr);
    };
} // namespace tracey
