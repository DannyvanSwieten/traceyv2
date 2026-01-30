#pragma once

#include "../core/types.hpp"
#include "attribute.hpp"
#include <vector>
#include <memory>

namespace tracey
{
    // Forward declaration
    class SceneObject;

    /**
     * @brief Pure geometric data - what flows through SOP nodes
     *
     * This is the fundamental geometry type in the procedural system.
     * Contains points, primitives (via indices), and attributes.
     * This is separate from SceneObject, which is a scene-level container.
     */
    class Geometry
    {
    public:
        Geometry() = default;
        ~Geometry() = default;

        // Allow move, delete copy (due to AttributeSet)
        Geometry(const Geometry&) = delete;
        Geometry& operator=(const Geometry&) = delete;
        Geometry(Geometry&&) = default;
        Geometry& operator=(Geometry&&) = default;

        // Point data (positions)
        const std::vector<Vec3>& positions() const { return m_positions; }
        std::vector<Vec3>& positions() { return m_positions; }
        void setPositions(std::vector<Vec3> positions) { m_positions = std::move(positions); }

        // Primitive data (indices for triangles)
        const std::vector<uint32_t>& indices() const { return m_indices; }
        std::vector<uint32_t>& indices() { return m_indices; }
        void setIndices(std::vector<uint32_t> indices) { m_indices = std::move(indices); }

        // Built-in attributes (for convenience)
        const std::vector<Vec3>& normals() const { return m_normals; }
        std::vector<Vec3>& normals() { return m_normals; }
        void setNormals(std::vector<Vec3> normals) { m_normals = std::move(normals); }

        const std::vector<Vec2>& uvs() const { return m_uvs; }
        std::vector<Vec2>& uvs() { return m_uvs; }
        void setUvs(std::vector<Vec2> uvs) { m_uvs = std::move(uvs); }

        // Query methods
        bool hasIndices() const { return !m_indices.empty(); }
        bool hasNormals() const { return !m_normals.empty(); }
        bool hasUvs() const { return !m_uvs.empty(); }

        size_t pointCount() const { return m_positions.size(); }
        size_t primitiveCount() const { return m_indices.size() / 3; }

        // Attribute system (for VOP-level operations)
        AttributeSet& attributes() { return m_attributes; }
        const AttributeSet& attributes() const { return m_attributes; }

        // Primitive generators (static factory methods)
        static Geometry createCube(float size = 1.0f);
        static Geometry createPlane(float width = 1.0f, float depth = 1.0f);
        static Geometry createSphere(float radius = 1.0f, uint32_t segments = 16, uint32_t rings = 16);
        static Geometry createTorus(float majorRadius = 1.0f, float minorRadius = 0.3f,
                                   uint32_t majorSegments = 32, uint32_t minorSegments = 16);
        static Geometry createCylinder(float radius = 0.5f, float height = 1.0f, uint32_t segments = 32);
        static Geometry createCone(float radius = 0.5f, float height = 1.0f, uint32_t segments = 32);

        // Convert to SceneObject for rendering
        class SceneObject toSceneObject(const std::string& name = "geometry") const;

    private:
        std::vector<Vec3> m_positions;      // Point positions (P attribute in Houdini)
        std::vector<uint32_t> m_indices;    // Primitive vertex indices
        std::vector<Vec3> m_normals;        // Built-in N attribute
        std::vector<Vec2> m_uvs;            // Built-in uv attribute
        AttributeSet m_attributes;          // Generic attribute storage
    };

} // namespace tracey
