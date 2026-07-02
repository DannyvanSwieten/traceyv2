#pragma once

#include "../core/types.hpp"
#include "light.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tracey
{
    class Scene;

    // A live, retained, composed USD *shot*: a root assembly layer that sublayers one
    // file per **department** (strength-ordered, front = strongest), plus an active
    // **edit target**. Department authoring routes opinions (`def`/`over`) into the
    // active department's layer only — so departments never clobber each other and the
    // composition resolves by the sublayer strength order. `toScene()` composes the
    // whole stage into a tracey::Scene for the renderer (the round-trip bridge).
    //
    // This is the document model for the USD-native editor: a workspace tab selects a
    // department (the edit target); the viewport always shows the composed result. See
    // docs/pipeline_layout.md and .claude/plans/sleepy-roaming-sloth.md (Phase 1).
    //
    // USD-free header (pimpl) so the editor includes it without pulling pxr; the
    // implementation lives in the tracey_usd library. When built without
    // TRACEY_WITH_USD, the factories return nullptr and available() is false.
    class StageDocument
    {
    public:
        ~StageDocument();
        StageDocument(StageDocument &&) noexcept;
        StageDocument &operator=(StageDocument &&) noexcept;

        // True when this build links OpenUSD.
        static bool available();

        // Create a new shot assembly at `shotPath` with one empty layer per department
        // (file "<dir>/<dept>.usda"), sublayered in the given order — front = strongest
        // (e.g. {"lighting","anim","layout"}). Returns null on failure / no USD.
        static std::unique_ptr<StageDocument> createShot(
            const std::string &shotPath, const std::vector<std::string> &departments);

        // Open an existing shot.usd, recovering its department layers from the root
        // layer's sublayers (department name = each sublayer's file stem).
        static std::unique_ptr<StageDocument> openShot(const std::string &shotPath);

        const std::vector<std::string> &departments() const; // strongest first
        const std::string &activeDepartment() const;
        // A human label for the shot (e.g. "seq01/sh01"), derived from its path.
        std::string shotName() const;
        // The time codes (frame numbers) at which `primPath` has authored transform
        // keyframes (xformOp time samples) — for drawing key markers on the timeline.
        std::vector<double> primKeyTimes(const std::string &primPath) const;
        bool setActiveDepartment(const std::string &dept); // routes authoring here
        std::string layerPath(const std::string &dept) const;

        // ── Authoring (into the active department's layer) ──────────────────
        // Reference an asset under `primPath` (layout): defines a prim there that
        // references `assetPath`'s default prim; downstream departments `over` prims
        // inside it by path.
        bool referenceAsset(const std::string &primPath, const std::string &assetPath);
        // Reference `assetPath` under a UNIQUE /shot/<baseName> prim in the active
        // layer (uniquified so referencing the same asset twice yields distinct
        // placements). Returns the prim path used, or "" on failure.
        std::string referenceAssetAuto(const std::string &assetPath, const std::string &baseName);
        // Remove the asset INSTANCE that `primPath` belongs to — climbs from the (mesh)
        // prim up to the instance root (the direct child of the shot's default prim) and
        // removes it from the layout layer, so it composes out and stays gone. Returns
        // true on success. Backs Layout-department actor deletion.
        bool removePrim(const std::string &primPath);
        // Author a TRS transform on `primPath` (anim / layout) as translate, rotateXYZ
        // (degrees), scale ops — an `over` when the prim is defined elsewhere.
        bool setPrimTransform(const std::string &primPath, const Vec3 &translate,
                              const Vec3 &rotateEulerDeg, const Vec3 &scale);
        // Author a single matrix xformOp on `primPath` (lossless — used to route a
        // viewport/gizmo edit's full transform without a quaternion→euler conversion).
        bool setPrimMatrix(const std::string &primPath, const Mat4 &localToParent);
        // Author a matrix xformOp TIME SAMPLE at `timeCode` (a frame number) on
        // `primPath` — a keyframe. Repeated samples interpolate on playback. Authored
        // into the active department layer (anim).
        bool setPrimMatrixAtTime(const std::string &primPath, double timeCode, const Mat4 &localToParent);
        // Author TRS xformOp TIME SAMPLES at `timeCode`: translate + rotateXYZ (euler
        // DEGREES) + scale. Rotation is keyed as EULER so it interpolates in euler space
        // — a -180°→180° key sweeps a full 360°, where a baked-matrix key treats the two
        // identical orientations as no motion. This is the shot-mode transform keyer.
        bool setPrimTRSAtTime(const std::string &primPath, double timeCode,
                              const Vec3 &translate, const Vec3 &rotateEulerDeg, const Vec3 &scale);
        // Author (or RE-author) an editor light at `primPath` as its matching UsdLux
        // type with its full parameter set + transform (lighting). Point→SphereLight,
        // Distant→DistantLight, Area→RectLight, Dome→DomeLight — the matching type is
        // load-bearing: convertLight maps them back, so anything else degrades on
        // recompose (the original bug: a Dome authored as SphereLight came back a dim
        // Point). Re-authoring with a different type replaces the old prim spec, so
        // this is also the light-EDIT sync (param tweaks + type changes persist).
        // Dome gradient colors ride as custom `tracey:*` attrs (no UsdLux equivalent).
        bool defineLight(const std::string &primPath, const Light &light, const Mat4 &localToWorld);

        // ── Bridge + persistence ────────────────────────────────────────────
        std::unique_ptr<Scene> toScene() const; // composed stage → Scene (render bridge)
        // Composed stage → Scene evaluated at `timeCode` (a frame number) — the render
        // bridge for shot playback / scrubbing of time-sampled animation.
        std::unique_ptr<Scene> toSceneAtTime(double timeCode) const;
        bool save() const;                       // save the root + all department layers

    private:
        StageDocument();
        struct Impl;
        std::unique_ptr<Impl> m;
    };
} // namespace tracey
