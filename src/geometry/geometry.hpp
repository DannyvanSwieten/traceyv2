#pragma once

#include "../core/types.hpp"
#include "attribute_table.hpp"

#include <cstdint>
#include <vector>

namespace tracey
{
    // Topology primitive. v1 only emits triangles, but the offset/count
    // representation is forward-compatible with quads and n-gons (Houdini's
    // GA_Primitive).
    struct GeoPrimitive
    {
        uint32_t firstVertex = 0;
        uint32_t vertexCount = 0;
    };

    // Houdini-style geometry container.
    //
    //   Points       indexed by point id;       attribute store: m_pointAttrs
    //   Vertices     indexed by vertex id;      attribute store: m_vertexAttrs
    //                each vertex references a point via m_vertexToPoint
    //   Primitives   indexed by primitive id;   attribute store: m_primAttrs
    //                each primitive owns a contiguous range of vertices
    //   Detail       always size 1;             attribute store: m_detailAttrs
    //
    // P (Vec3 point attribute, position) is always present after construction.
    class Geometry
    {
    public:
        Geometry();

        Geometry(const Geometry &other) = default;
        Geometry &operator=(const Geometry &other) = default;
        Geometry(Geometry &&) noexcept = default;
        Geometry &operator=(Geometry &&) noexcept = default;

        Geometry clone() const { return *this; }

        // ── Point ops ──
        size_t pointCount() const { return m_pointAttrs.size(); }
        // Append a new point at position p. Returns the new point id.
        // All existing point attributes get their defaultValue() in the new slot.
        size_t addPoint(const Vec3 &p);
        // Bulk-resize the point store (existing attributes filled with defaults).
        void resizePoints(size_t n) { m_pointAttrs.resize(n); }

        // ── Vertex / primitive ops ──
        size_t vertexCount() const { return m_vertexAttrs.size(); }
        size_t primitiveCount() const { return m_primAttrs.size(); }

        // Append a triangle that references the given three point ids. Adds
        // three vertices (referencing those points), one primitive (covering
        // those vertices). Returns the new primitive id.
        size_t addTriangle(uint32_t p0, uint32_t p1, uint32_t p2);

        // ── Tables (mutable) ──
        AttributeTable &points()    { return m_pointAttrs; }
        AttributeTable &vertices()  { return m_vertexAttrs; }
        AttributeTable &primitives() { return m_primAttrs; }
        AttributeTable &detail()    { return m_detailAttrs; }

        const AttributeTable &points()    const { return m_pointAttrs; }
        const AttributeTable &vertices()  const { return m_vertexAttrs; }
        const AttributeTable &primitives() const { return m_primAttrs; }
        const AttributeTable &detail()    const { return m_detailAttrs; }

        // Vertex → point connectivity table. Always size = vertexCount().
        const std::vector<uint32_t> &vertexToPoint() const { return m_vertexToPoint; }
        std::vector<uint32_t> &vertexToPoint() { return m_vertexToPoint; }

        const std::vector<GeoPrimitive> &primitivesList() const { return m_primitivesList; }
        std::vector<GeoPrimitive> &primitivesList() { return m_primitivesList; }

        // Append all of `other` into `this`. Point/vertex/primitive ids in
        // `other` are remapped (offset by current sizes); attribute tables
        // merge by name (matching attributes get copied; non-matching are
        // dropped on the appended side).
        void mergeFrom(const Geometry &other);

        // ── Convenience accessors (P is mandatory) ──
        std::vector<Vec3> &positions();
        const std::vector<Vec3> &positions() const;

    private:
        AttributeTable m_pointAttrs;
        AttributeTable m_vertexAttrs;
        AttributeTable m_primAttrs;
        AttributeTable m_detailAttrs;

        std::vector<uint32_t> m_vertexToPoint;
        std::vector<GeoPrimitive> m_primitivesList;
    };
}
