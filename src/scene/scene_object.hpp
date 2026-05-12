#pragma once
#include "../core/types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace tracey
{
    class SceneObject
    {
    public:
        SceneObject() = default;
        SceneObject(const std::string &name);

        const std::string &name() const { return m_name; }
        void setName(const std::string &name) { m_name = name; }

        const std::vector<Vec3> &positions() const { return m_positions; }
        const std::vector<uint32_t> &indices() const { return m_indices; }
        const std::vector<Vec3> &normals() const { return m_normals; }
        const std::vector<Vec2> &uvs() const { return m_uvs; }
        // Per-vertex color (Cd), one entry per position. Set by the SOP→
        // SceneObject converter when the Geometry carries a "Cd" Point
        // attribute; the rasterizer streams it as a second vertex buffer
        // and multiplies it into the material base color in the shader.
        const std::vector<Vec3> &colors() const { return m_colors; }

        void setPositions(std::vector<Vec3> positions) { m_positions = std::move(positions); }
        void setIndices(std::vector<uint32_t> indices) { m_indices = std::move(indices); }
        void setNormals(std::vector<Vec3> normals) { m_normals = std::move(normals); }
        void setUvs(std::vector<Vec2> uvs) { m_uvs = std::move(uvs); }
        void setColors(std::vector<Vec3> colors) { m_colors = std::move(colors); }

        bool hasIndices() const { return !m_indices.empty(); }
        bool hasNormals() const { return !m_normals.empty(); }
        bool hasUvs() const { return !m_uvs.empty(); }
        bool hasColors() const { return !m_colors.empty(); }

        size_t vertexCount() const { return m_positions.size(); }
        size_t triangleCount() const;

        // Primitive generators
        static SceneObject createCube(float size = 1.0f);
        // `cols` segments along X (width), `rows` along Z (depth). 1×1 = 2
        // triangles (the legacy result); larger values produce a uniform grid
        // useful for displacement / wireframe testing.
        static SceneObject createPlane(float width = 1.0f, float depth = 1.0f,
                                       uint32_t cols = 1, uint32_t rows = 1);
        static SceneObject createSphere(float radius = 1.0f, uint32_t segments = 16, uint32_t rings = 16);
        static SceneObject createTorus(float majorRadius = 1.0f, float minorRadius = 0.3f,
                                       uint32_t majorSegments = 32, uint32_t minorSegments = 16);
        static SceneObject createCylinder(float radius = 0.5f, float height = 1.0f, uint32_t segments = 32);
        static SceneObject createCone(float radius = 0.5f, float height = 1.0f, uint32_t segments = 32);

    private:
        std::string m_name;
        std::vector<Vec3> m_positions;
        std::vector<uint32_t> m_indices;
        std::vector<Vec3> m_normals;
        std::vector<Vec2> m_uvs;
        std::vector<Vec3> m_colors;

        // Future extension point for geometry modification graphs
        // std::unique_ptr<GeometryGraph> m_modifierGraph;
    };
}
