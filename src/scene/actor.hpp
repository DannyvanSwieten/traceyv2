#pragma once
#include <span>
#include <string>
#include <vector>
#include "transform.hpp"
#include "scene_instance.hpp"

namespace tracey
{
    class Scene;
    class Actor
    {
    public:
        ~Actor() = default;

        Actor(const Actor &) = delete;
        Actor &operator=(const Actor &) = delete;

        const std::string &name() const { return m_name; }
        void setName(const std::string &name) { m_name = name; }

        size_t getUid() const { return uid; }

        void setTransform(const Transform &transform);
        void applyTransform(const Transform &deltaTransform);
        const Transform &transform() const;
        const std::span<const size_t> children() const;
        void addChild(Actor *child);
        void removeChild(size_t childUid);

        // Parent tracking
        bool hasParent() const { return m_parent != INVALID_PARENT; }
        size_t parent() const { return m_parent; }
        void setParent(size_t parentUid) { m_parent = parentUid; }
        void clearParent() { m_parent = INVALID_PARENT; }

        static constexpr size_t INVALID_PARENT = static_cast<size_t>(-1);

        // Instance management
        void addInstance(const SceneInstance &instance) { m_instances.push_back(instance); }
        void addInstance(SceneInstance &&instance) { m_instances.push_back(std::move(instance)); }
        const std::vector<SceneInstance> &instances() const { return m_instances; }
        std::vector<SceneInstance> &instances() { return m_instances; }
        void clearInstances() { m_instances.clear(); }

        Actor(Scene *scene, size_t uid) : m_scene(scene), uid(uid) {}

    private:
        [[maybe_unused]] Scene *m_scene = nullptr;
        size_t uid = 0;
        size_t m_parent = INVALID_PARENT;
        std::string m_name;
        Transform m_transform;
        std::vector<size_t> m_children;
        std::vector<SceneInstance> m_instances;
    };
}