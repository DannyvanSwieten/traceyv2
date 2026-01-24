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

    SceneObject SceneObject::createTorus(float majorRadius, float minorRadius,
                                         uint32_t majorSegments, uint32_t minorSegments)
    {
        SceneObject obj("torus");
        const float PI = 3.14159265359f;
        const float TWO_PI = 2.0f * PI;

        // Generate vertices for a torus
        // u goes around the major circle, v goes around the minor circle (tube)
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<Vec2> uvs;

        for (uint32_t i = 0; i <= majorSegments; ++i)
        {
            float u = TWO_PI * static_cast<float>(i) / static_cast<float>(majorSegments);
            float cosU = std::cos(u);
            float sinU = std::sin(u);

            for (uint32_t j = 0; j <= minorSegments; ++j)
            {
                float v = TWO_PI * static_cast<float>(j) / static_cast<float>(minorSegments);
                float cosV = std::cos(v);
                float sinV = std::sin(v);

                // Position on torus surface
                float x = (majorRadius + minorRadius * cosV) * cosU;
                float y = minorRadius * sinV;
                float z = (majorRadius + minorRadius * cosV) * sinU;

                // Normal (points outward from tube center)
                Vec3 normal(cosV * cosU, sinV, cosV * sinU);

                positions.push_back(Vec3(x, y, z));
                normals.push_back(normal);
                uvs.push_back(Vec2(
                    static_cast<float>(i) / static_cast<float>(majorSegments),
                    static_cast<float>(j) / static_cast<float>(minorSegments)));
            }
        }

        // Generate triangles (expand to non-indexed format)
        for (uint32_t i = 0; i < majorSegments; ++i)
        {
            for (uint32_t j = 0; j < minorSegments; ++j)
            {
                uint32_t i0 = i * (minorSegments + 1) + j;
                uint32_t i1 = i0 + 1;
                uint32_t i2 = i0 + minorSegments + 1;
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

    SceneObject SceneObject::createCylinder(float radius, float height, uint32_t segments)
    {
        SceneObject obj("cylinder");
        const float PI = 3.14159265359f;
        const float TWO_PI = 2.0f * PI;
        float halfHeight = height * 0.5f;

        // Generate side vertices
        for (uint32_t i = 0; i < segments; ++i)
        {
            float theta0 = TWO_PI * static_cast<float>(i) / static_cast<float>(segments);
            float theta1 = TWO_PI * static_cast<float>(i + 1) / static_cast<float>(segments);

            float cos0 = std::cos(theta0);
            float sin0 = std::sin(theta0);
            float cos1 = std::cos(theta1);
            float sin1 = std::sin(theta1);

            Vec3 p0(radius * cos0, -halfHeight, radius * sin0);
            Vec3 p1(radius * cos1, -halfHeight, radius * sin1);
            Vec3 p2(radius * cos1, halfHeight, radius * sin1);
            Vec3 p3(radius * cos0, halfHeight, radius * sin0);

            Vec3 n0(cos0, 0, sin0);
            Vec3 n1(cos1, 0, sin1);

            float u0 = static_cast<float>(i) / static_cast<float>(segments);
            float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);

            // First triangle
            obj.m_positions.push_back(p0);
            obj.m_positions.push_back(p1);
            obj.m_positions.push_back(p2);
            obj.m_normals.push_back(n0);
            obj.m_normals.push_back(n1);
            obj.m_normals.push_back(n1);
            obj.m_uvs.push_back(Vec2(u0, 0));
            obj.m_uvs.push_back(Vec2(u1, 0));
            obj.m_uvs.push_back(Vec2(u1, 1));

            // Second triangle
            obj.m_positions.push_back(p0);
            obj.m_positions.push_back(p2);
            obj.m_positions.push_back(p3);
            obj.m_normals.push_back(n0);
            obj.m_normals.push_back(n1);
            obj.m_normals.push_back(n0);
            obj.m_uvs.push_back(Vec2(u0, 0));
            obj.m_uvs.push_back(Vec2(u1, 1));
            obj.m_uvs.push_back(Vec2(u0, 1));
        }

        // Top cap
        Vec3 topCenter(0, halfHeight, 0);
        Vec3 topNormal(0, 1, 0);
        for (uint32_t i = 0; i < segments; ++i)
        {
            float theta0 = TWO_PI * static_cast<float>(i) / static_cast<float>(segments);
            float theta1 = TWO_PI * static_cast<float>(i + 1) / static_cast<float>(segments);

            Vec3 p0(radius * std::cos(theta0), halfHeight, radius * std::sin(theta0));
            Vec3 p1(radius * std::cos(theta1), halfHeight, radius * std::sin(theta1));

            obj.m_positions.push_back(topCenter);
            obj.m_positions.push_back(p0);
            obj.m_positions.push_back(p1);
            obj.m_normals.push_back(topNormal);
            obj.m_normals.push_back(topNormal);
            obj.m_normals.push_back(topNormal);
            obj.m_uvs.push_back(Vec2(0.5f, 0.5f));
            obj.m_uvs.push_back(Vec2(0.5f + 0.5f * std::cos(theta0), 0.5f + 0.5f * std::sin(theta0)));
            obj.m_uvs.push_back(Vec2(0.5f + 0.5f * std::cos(theta1), 0.5f + 0.5f * std::sin(theta1)));
        }

        // Bottom cap
        Vec3 bottomCenter(0, -halfHeight, 0);
        Vec3 bottomNormal(0, -1, 0);
        for (uint32_t i = 0; i < segments; ++i)
        {
            float theta0 = TWO_PI * static_cast<float>(i) / static_cast<float>(segments);
            float theta1 = TWO_PI * static_cast<float>(i + 1) / static_cast<float>(segments);

            Vec3 p0(radius * std::cos(theta0), -halfHeight, radius * std::sin(theta0));
            Vec3 p1(radius * std::cos(theta1), -halfHeight, radius * std::sin(theta1));

            obj.m_positions.push_back(bottomCenter);
            obj.m_positions.push_back(p1);
            obj.m_positions.push_back(p0);
            obj.m_normals.push_back(bottomNormal);
            obj.m_normals.push_back(bottomNormal);
            obj.m_normals.push_back(bottomNormal);
            obj.m_uvs.push_back(Vec2(0.5f, 0.5f));
            obj.m_uvs.push_back(Vec2(0.5f + 0.5f * std::cos(theta1), 0.5f + 0.5f * std::sin(theta1)));
            obj.m_uvs.push_back(Vec2(0.5f + 0.5f * std::cos(theta0), 0.5f + 0.5f * std::sin(theta0)));
        }

        return obj;
    }

    SceneObject SceneObject::createCone(float radius, float height, uint32_t segments)
    {
        SceneObject obj("cone");
        const float PI = 3.14159265359f;
        const float TWO_PI = 2.0f * PI;
        float halfHeight = height * 0.5f;

        // Slope for normals
        float slope = radius / height;
        float normalY = 1.0f / std::sqrt(1.0f + slope * slope);
        float normalXZ = slope * normalY;

        // Apex
        Vec3 apex(0, halfHeight, 0);

        // Generate side triangles
        for (uint32_t i = 0; i < segments; ++i)
        {
            float theta0 = TWO_PI * static_cast<float>(i) / static_cast<float>(segments);
            float theta1 = TWO_PI * static_cast<float>(i + 1) / static_cast<float>(segments);

            float cos0 = std::cos(theta0);
            float sin0 = std::sin(theta0);
            float cos1 = std::cos(theta1);
            float sin1 = std::sin(theta1);

            Vec3 p0(radius * cos0, -halfHeight, radius * sin0);
            Vec3 p1(radius * cos1, -halfHeight, radius * sin1);

            // Average normal for the apex vertex (pointing up and out)
            float midTheta = (theta0 + theta1) * 0.5f;
            Vec3 apexNormal(normalXZ * std::cos(midTheta), normalY, normalXZ * std::sin(midTheta));
            Vec3 n0(normalXZ * cos0, normalY, normalXZ * sin0);
            Vec3 n1(normalXZ * cos1, normalY, normalXZ * sin1);

            obj.m_positions.push_back(p0);
            obj.m_positions.push_back(p1);
            obj.m_positions.push_back(apex);
            obj.m_normals.push_back(n0);
            obj.m_normals.push_back(n1);
            obj.m_normals.push_back(apexNormal);

            float u0 = static_cast<float>(i) / static_cast<float>(segments);
            float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);
            obj.m_uvs.push_back(Vec2(u0, 0));
            obj.m_uvs.push_back(Vec2(u1, 0));
            obj.m_uvs.push_back(Vec2((u0 + u1) * 0.5f, 1));
        }

        // Bottom cap
        Vec3 bottomCenter(0, -halfHeight, 0);
        Vec3 bottomNormal(0, -1, 0);
        for (uint32_t i = 0; i < segments; ++i)
        {
            float theta0 = TWO_PI * static_cast<float>(i) / static_cast<float>(segments);
            float theta1 = TWO_PI * static_cast<float>(i + 1) / static_cast<float>(segments);

            Vec3 p0(radius * std::cos(theta0), -halfHeight, radius * std::sin(theta0));
            Vec3 p1(radius * std::cos(theta1), -halfHeight, radius * std::sin(theta1));

            obj.m_positions.push_back(bottomCenter);
            obj.m_positions.push_back(p1);
            obj.m_positions.push_back(p0);
            obj.m_normals.push_back(bottomNormal);
            obj.m_normals.push_back(bottomNormal);
            obj.m_normals.push_back(bottomNormal);
            obj.m_uvs.push_back(Vec2(0.5f, 0.5f));
            obj.m_uvs.push_back(Vec2(0.5f + 0.5f * std::cos(theta1), 0.5f + 0.5f * std::sin(theta1)));
            obj.m_uvs.push_back(Vec2(0.5f + 0.5f * std::cos(theta0), 0.5f + 0.5f * std::sin(theta0)));
        }

        return obj;
    }
}
