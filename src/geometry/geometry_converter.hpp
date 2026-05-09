#pragma once

#include "geometry.hpp"

namespace tracey
{
    class SceneObject;

    // Bridges between the SOP-side `Geometry` (attribute tables, indexed
    // primitives) and the renderer-side `SceneObject` (flat per-corner pos /
    // normal / uv arrays + optional indices). The path tracer's BVH
    // compilation pipeline keeps consuming SceneObject — this converter is
    // what lets the SOP graph produce something the existing renderer can
    // ingest unchanged.
    class GeometryConverter
    {
    public:
        // Geometry → SceneObject.
        //
        //   Position: per-corner from the point P attribute (looked up via
        //             vertexToPoint).
        //   Normal:   if a Vertex-class N exists, used per-corner. Else if a
        //             Point-class N exists, looked up via vertexToPoint.
        //             Else: SceneObject normals stay empty (renderer
        //             computes flat normals or treats as missing).
        //   UV:       Vertex-class "uv" wins; falls back to Point-class.
        //
        // Output is non-indexed (matches existing primitive generators):
        // positions/normals/uvs are flat per-corner arrays; indices stay
        // empty.
        static SceneObject toSceneObject(const Geometry &geo, const std::string &name);

        // SceneObject → Geometry.
        //
        // No deduplication: every face-corner becomes its own point. Lets the
        // converter stay simple for v1; a future Fuse SOP can weld coincident
        // points later.
        //
        //   P (Vec3, Point) is filled from positions.
        //   If normals are present, an N (Vec3, Point) attribute is added.
        //   If uvs are present, a uv (Vec2, Vertex) attribute is added.
        static Geometry fromSceneObject(const SceneObject &obj);
    };
}
