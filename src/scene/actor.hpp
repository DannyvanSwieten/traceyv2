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
        void addChild(Actor *child)
        {
            m_children.push_back(child->uid);
        }
        void removeChild(size_t childUid);

        // Instance management
        void addInstance(const SceneInstance &instance) { m_instances.push_back(instance); }
        void addInstance(SceneInstance &&instance) { m_instances.push_back(std::move(instance)); }
        const std::vector<SceneInstance> &instances() const { return m_instances; }
        std::vector<SceneInstance> &instances() { return m_instances; }
        void clearInstances() { m_instances.clear(); }

        // ShaderGraph JSON attached to this actor. Empty string means
        // "use the default passthrough program". SceneCompiler aggregates
        // unique JSON strings across actors into the MaterialProgramBuffer
        // and emits one program per unique graph. The editor populates this
        // by reading from its per-user material library (a name -> file map
        // editor-side); the engine itself doesn't know about that catalog.
        const std::string &materialGraphJson() const { return m_materialGraphJson; }
        void setMaterialGraphJson(const std::string &json) { m_materialGraphJson = json; }

        Actor(Scene *scene, size_t uid) : m_scene(scene), uid(uid) {}

    private:
        [[maybe_unused]] Scene *m_scene = nullptr;
        size_t uid = 0;
        std::string m_name;
        Transform m_transform;
        std::vector<size_t> m_children;
        std::vector<SceneInstance> m_instances;
        std::string m_materialGraphJson;
    };
}