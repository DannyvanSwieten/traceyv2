#include "scene_object.hpp"
#include <cmath>

namespace tracey
{
    SceneObject::SceneObject(const std::string &name)
        : m_name(name)
    {
    }

    size_t SceneObject::triangleCount() const
    {
        if (!m_indices.empty())
        {
            return m_indices.size() / 3;
        }
        return m_positions.size() / 3;
    }

    SceneObject SceneObject::createCube(float size)
    {
        SceneObject obj("cube");
        float h = size * 0.5f;

        // 36 vertices (6 faces * 2 triangles * 3 vertices)
        // Each face has its own vertices for proper normals
        obj.m_positions = {
            // Front face (Z+)
            Vec3(-h, -h, h), Vec3(h, -h, h), Vec3(h, h, h),
            Vec3(-h, -h, h), Vec3(h, h, h), Vec3(-h, h, h),
            // Back face (Z-)
            Vec3(h, -h, -h), Vec3(-h, -h, -h), Vec3(-h, h, -h),
            Vec3(h, -h, -h), Vec3(-h, h, -h), Vec3(h, h, -h),
            // Left face (X-)
            Vec3(-h, -h, -h), Vec3(-h, -h, h), Vec3(-h, h, h),
            Vec3(-h, -h, -h), Vec3(-h, h, h), Vec3(-h, h, -h),
            // Right face (X+)
            Vec3(h, -h, h), Vec3(h, -h, -h), Vec3(h, h, -h),
            Vec3(h, -h, h), Vec3(h, h, -h), Vec3(h, h, h),
            // Top face (Y+)
            Vec3(-h, h, h), Vec3(h, h, h), Vec3(h, h, -h),
            Vec3(-h, h, h), Vec3(h, h, -h), Vec3(-h, h, -h),
            // Bottom face (Y-)
            Vec3(-h, -h, -h), Vec3(h, -h, -h), Vec3(h, -h, h),
            Vec3(-h, -h, -h), Vec3(h, -h, h), Vec3(-h, -h, h),
        };

        obj.m_normals = {
            // Front face
            Vec3(0, 0, 1), Vec3(0, 0, 1), Vec3(0, 0, 1),
            Vec3(0, 0, 1), Vec3(0, 0, 1), Vec3(0, 0, 1),
            // Back face
            Vec3(0, 0, -1), Vec3(0, 0, -1), Vec3(0, 0, -1),
            Vec3(0, 0, -1), Vec3(0, 0, -1), Vec3(0, 0, -1),
            // Left face
            Vec3(-1, 0, 0), Vec3(-1, 0, 0), Vec3(-1, 0, 0),
            Vec3(-1, 0, 0), Vec3(-1, 0, 0), Vec3(-1, 0, 0),
            // Right face
            Vec3(1, 0, 0), Vec3(1, 0, 0), Vec3(1, 0, 0),
            Vec3(1, 0, 0), Vec3(1, 0, 0), Vec3(1, 0, 0),
            // Top face
            Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 1, 0),
            Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 1, 0),
            // Bottom face
            Vec3(0, -1, 0), Vec3(0, -1, 0), Vec3(0, -1, 0),
            Vec3(0, -1, 0), Vec3(0, -1, 0), Vec3(0, -1, 0),
        };

        obj.m_uvs = {
            // Front face
            Vec2(0, 0), Vec2(1, 0), Vec2(1, 1),
            Vec2(0, 0), Vec2(1, 1), Vec2(0, 1),
            // Back face
            Vec2(0, 0), Vec2(1, 0), Vec2(1, 1),
            Vec2(0, 0), Vec2(1, 1), Vec2(0, 1),
            // Left face
            Vec2(0, 0), Vec2(1, 0), Vec2(1, 1),
            Vec2(0, 0), Vec2(1, 1), Vec2(0, 1),
            // Right face
            Vec2(0, 0), Vec2(1, 0), Vec2(1, 1),
            Vec2(0, 0), Vec2(1, 1), Vec2(0, 1),
            // Top face
            Vec2(0, 0), Vec2(1, 0), Vec2(1, 1),
            Vec2(0, 0), Vec2(1, 1), Vec2(0, 1),
            // Bottom face
            Vec2(0, 0), Vec2(1, 0), Vec2(1, 1),
            Vec2(0, 0), Vec2(1, 1), Vec2(0, 1),
        };

        return obj;
    }

    SceneObject SceneObject::createPlane(float width, float depth)
    {
        SceneObject obj("plane");
        float hw = width * 0.5f;
        float hd = depth * 0.5f;

        // Two triangles forming a plane in XZ, facing up (Y+)
        obj.m_positions = {
            Vec3(-hw, 0, -hd), Vec3(hw, 0, -hd), Vec3(hw, 0, hd),
            Vec3(-hw, 0, -hd), Vec3(hw, 0, hd), Vec3(-hw, 0, hd),
        };

        obj.m_normals = {
            Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 1, 0),
            Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 1, 0),
        };

        obj.m_uvs = {
            Vec2(0, 0), Vec2(1, 0), Vec2(1, 1),
            Vec2(0, 0), Vec2(1, 1), Vec2(0, 1),
        };

        return obj;
    }

    SceneObject SceneObject::createSphere(float radius, uint32_t segments, uint32_t rings)
    {
        SceneObject obj("sphere");
        const float PI = 3.14159265359f;

        // Generate vertices
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<Vec2> uvs;

        for (uint32_t ring = 0; ring <= rings; ++ring)
        {
            float phi = PI * static_cast<float>(ring) / static_cast<float>(rings);
            float sinPhi = std::sin(phi);
            float cosPhi = std::cos(phi);

            for (uint32_t seg = 0; seg <= segments; ++seg)
            {
                float theta = 2.0f * PI * static_cast<float>(seg) / static_cast<float>(segments);
                float sinTheta = std::sin(theta);
                float cosTheta = std::cos(theta);

                Vec3 normal(sinPhi * cosTheta, cosPhi, sinPhi * sinTheta);
                Vec3 pos = normal * radius;

                positions.push_back(pos);
                normals.push_back(normal);
                uvs.push_back(Vec2(
                    static_cast<float>(seg) / static_cast<float>(segments),
                    static_cast<float>(ring) / static_cast<float>(rings)));
            }
        }

        // Generate indices and expand to non-indexed format
        for (uint32_t ring = 0; ring < rings; ++ring)
        {
            for (uint32_t seg = 0; seg < segments; ++seg)
            {
                uint32_t i0 = ring * (segments + 1) + seg;
                uint32_t i1 = i0 + 1;
                uint32_t i2 = i0 + segments + 1;
                uint32_t i3 = i2 + 1;

                // First triangle
                obj.m_positions.push_back(positions[i0]);
                obj.m_positions.push_back(positions[i2]);
                obj.m_positions.push_back(positions[i1]);
                obj.m_normals.push_back(normals[i0]);
                obj.m_normals.push_back(normals[i2]);
                obj.m_normals.push_back(normals[i1]);
                obj.m_uvs.push_back(uvs[i0]);
                obj.m_uvs.push_back(uvs[i2]);
                obj.m_uvs.push_back(uvs[i1]);

                // Second triangle
                obj.m_positions.push_back(positions[i1]);
                obj.m_positions.push_back(positions[i2]);
                obj.m_positions.push_back(positions[i3]);
                obj.m_normals.push_back(normals[i1]);
                obj.m_normals.push_back(normals[i2]);
                obj.m_normals.push_back(normals[i3]);
                obj.m_uvs.push_back(uvs[i1]);
                obj.m_uvs.push_back(uvs[i2]);
                obj.m_uvs.push_back(uvs[i3]);
            }
        }

        return obj;
    }
}
