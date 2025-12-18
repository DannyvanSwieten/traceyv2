#pragma once
#include <span>
#include <vector>
#include "transform.hpp"

namespace tracey
{
    class Scene;
    class Actor
    {
    public:
        ~Actor() = default;

        Actor(const Actor &) = delete;
        Actor &operator=(const Actor &) = delete;

        void setTransform(const Transform &transform);
        void applyTransform(const Transform &deltaTransform);
        const Transform &transform() const;
        const std::span<const size_t> children() const;
        void addChild(Actor *child)
        {
            m_children.push_back(child->uid);
        }
        void removeChild(size_t childUid);

        Actor(Scene *scene, size_t uid) : m_scene(scene), uid(uid) {}

    private:
        [[maybe_unused]] Scene *m_scene = nullptr;
        size_t uid = 0;
        Transform m_transform;
        std::vector<size_t> m_children;
    };
}