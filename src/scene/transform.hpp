#pragma once
#include "../core/types.hpp"
namespace tracey
{
    class Transform
    {
    public:
        tracey::Mat4 toMatrix() const
        {
            tracey::Mat4 translationMat = glm::translate(m_position);
            tracey::Mat4 rotationMat = glm::mat4_cast(m_rotation);
            tracey::Mat4 scaleMat = glm::scale(m_scale);
            return translationMat * rotationMat * scaleMat;
        }

        void setPosition(const tracey::Vec3 &pos)
        {
            m_position = pos;
        }

        void setRotation(const tracey::Quaternion &rot)
        {
            m_rotation = rot;
        }

        void setScale(const tracey::Vec3 &s)
        {
            m_scale = s;
        }

        void applyRotation(const tracey::Quaternion &deltaRot)
        {
            m_rotation = deltaRot * m_rotation;
        }

        void applyScale(const tracey::Vec3 &deltaScale)
        {
            m_scale *= deltaScale;
        }

        tracey::Vec3 position() const
        {
            return m_position;
        }

        tracey::Quaternion rotation() const
        {
            return m_rotation;
        }

        tracey::Vec3 scale() const
        {
            return m_scale;
        }

    private:
        tracey::Vec3 m_position;
        tracey::Quaternion m_rotation;
        tracey::Vec3 m_scale;
    };
}