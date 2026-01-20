#pragma once
#include "../core/types.hpp"
#include "transform.hpp"

namespace tracey
{
    class Camera
    {
    public:
        Camera() = default;

        // Position and orientation
        void setPosition(const Vec3 &pos) { m_position = pos; }
        Vec3 position() const { return m_position; }

        void setRotation(const Quaternion &rot) { m_rotation = rot; }
        Quaternion rotation() const { return m_rotation; }

        // Camera parameters
        void setFov(float fovDegrees) { m_fov = fovDegrees; }
        float fov() const { return m_fov; }

        void setNearPlane(float near) { m_nearPlane = near; }
        float nearPlane() const { return m_nearPlane; }

        void setFarPlane(float far) { m_farPlane = far; }
        float farPlane() const { return m_farPlane; }

        void setAspectRatio(float aspect) { m_aspectRatio = aspect; }
        float aspectRatio() const { return m_aspectRatio; }

        // Computed directions
        Vec3 forward() const
        {
            return normalize(m_rotation * Vec3(0.0f, 0.0f, -1.0f));
        }

        Vec3 right() const
        {
            return normalize(m_rotation * Vec3(1.0f, 0.0f, 0.0f));
        }

        Vec3 up() const
        {
            return normalize(m_rotation * Vec3(0.0f, 1.0f, 0.0f));
        }

        // View matrix
        Mat4 viewMatrix() const
        {
            Vec3 fwd = forward();
            Vec3 u = up();
            return glm::lookAt(m_position, m_position + fwd, u);
        }

        /// Create a camera that frames the given bounding box
        /// @param minBounds Minimum corner of bounding box
        /// @param maxBounds Maximum corner of bounding box
        /// @param fovDegrees Field of view in degrees (default 45)
        /// @return Camera positioned to view the entire scene
        static Camera fitToBounds(const Vec3 &minBounds, const Vec3 &maxBounds, float fovDegrees = 45.0f);

    private:
        Vec3 m_position{0.0f, 0.0f, 0.0f};
        Quaternion m_rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity
        float m_fov = 45.0f;                           // degrees
        float m_nearPlane = 0.01f;
        float m_farPlane = 1000.0f;
        float m_aspectRatio = 1.0f;
    };
}
