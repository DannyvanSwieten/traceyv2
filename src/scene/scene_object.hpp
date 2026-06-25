#pragma once
#include "../core/types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace tracey
{
    class Skeleton;

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

        // Skinning data (one entry per position, kept 1:1 with positions even
        // after the loader expands an indexed primitive to a triangle list).
        // jointIndices are packed as floats (glTF JOINTS_0); weights are
        // normalized (WEIGHTS_0). The skeleton is the parsed rig + clips; a
        // skinning deformer needs only (joints, weights, skeleton, time).
        const std::vector<Vec4> &jointIndices() const { return m_jointIndices; }
        const std::vector<Vec4> &jointWeights() const { return m_jointWeights; }
        const std::shared_ptr<const Skeleton> &skeleton() const { return m_skeleton; }
        // inverse(meshNodeBindWorld) — premultiplied into the skinning matrices
        // so skinned vertices land in this mesh node's local space (the engine's
        // actor transform then places them in the world). Identity until set.
        const Mat4 &skinBindShift() const { return m_skinBindShift; }
        void setJointIndices(std::vector<Vec4> j) { m_jointIndices = std::move(j); }
        void setJointWeights(std::vector<Vec4> w) { m_jointWeights = std::move(w); }
        void setSkeleton(std::shared_ptr<const Skeleton> s) { m_skeleton = std::move(s); }
        void setSkinBindShift(const Mat4 &m) { m_skinBindShift = m; }

        bool hasIndices() const { return !m_indices.empty(); }
        bool hasNormals() const { return !m_normals.empty(); }
        bool hasUvs() const { return !m_uvs.empty(); }
        bool hasColors() const { return !m_colors.empty(); }
        // True only when full skinning data is present (weights + skeleton).
        bool hasSkin() const
        {
            return m_skeleton != nullptr && !m_jointIndices.empty() &&
                   m_jointWeights.size() == m_positions.size();
        }

        size_t vertexCount() const { return m_positions.size(); }
        size_t triangleCount() const;

        // 64-bit fingerprint of the data that goes into BLAS construction
        // (positions + indices). Stable across calls for the same data;
        // changes when geometry topology or vertex positions change. Used
        // by SceneCompiler's BlasCache to detect "this named object is
        // structurally unchanged since the last compile" and skip the
        // expensive BVH rebuild + GPU upload. Lazily memoised; mutators
        // clear the cached value.
        uint64_t contentHash() const;

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

        // Skinning (empty for non-skinned meshes).
        std::vector<Vec4> m_jointIndices;
        std::vector<Vec4> m_jointWeights;
        std::shared_ptr<const Skeleton> m_skeleton;
        Mat4 m_skinBindShift{1.0f}; // inverse(meshNodeBindWorld); identity until set

        // Future extension point for geometry modification graphs
        // std::unique_ptr<GeometryGraph> m_modifierGraph;
    };
}
