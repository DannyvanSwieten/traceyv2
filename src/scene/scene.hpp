#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <optional>
#include <cstdint>
#include "actor.hpp"
#include "scene_object.hpp"
#include "camera.hpp"
#include "procedural/node_graph.hpp"

namespace tracey
{
    struct SceneNode
    {
        Mat4 worldTransform;
        const Actor *const actor;
    };

    // Embedded texture data from GLTF/GLB files
    struct EmbeddedTexture
    {
        std::vector<unsigned char> data;
        int width = 0;
        int height = 0;
        int channels = 0;
        std::string mimeType;
    };

    class Scene
    {
    public:
        Scene();  // Creates root actor automatically
        ~Scene() = default;

        // Actor management
        Actor *createActor();                            // Create actor under root
        Actor *createActorUnderParent(size_t parentUid); // Create actor under specific parent
        Actor *createActorWithUid(size_t uid);           // Create actor with specific UID (for Rust interop)

        // Root access
        Actor *getRoot() { return m_root >= 0 ? getActor(static_cast<size_t>(m_root)) : nullptr; }
        const Actor *getRoot() const { return m_root >= 0 ? getActor(static_cast<size_t>(m_root)) : nullptr; }
        size_t getRootUid() const { return m_root >= 0 ? static_cast<size_t>(m_root) : 0; }
        bool hasRoot() const { return m_root >= 0; }
        Actor *getActor(size_t uid)
        {
            if (uid >= m_actors.size())
                return nullptr;
            return m_actors[uid].get();
        }

        const Actor *getActor(size_t uid) const
        {
            if (uid >= m_actors.size())
                return nullptr;
            return m_actors[uid].get();
        }

        void removeActor(size_t uid);
        void clear();
        std::vector<SceneNode> flatten() const;
        const std::vector<std::unique_ptr<Actor>> &actors() const { return m_actors; }

        // Object management
        void addObject(const std::string &name, std::unique_ptr<SceneObject> obj);
        void addObject(const std::string &name, SceneObject &&obj);
        SceneObject *getObject(const std::string &name);
        const SceneObject *getObject(const std::string &name) const;
        bool hasObject(const std::string &name) const;
        const std::unordered_map<std::string, std::unique_ptr<SceneObject>> &objects() const { return m_objects; }

        // Camera management
        void setCamera(const Camera &camera) { m_camera = camera; }
        bool hasCamera() const { return m_camera.has_value(); }
        const Camera &camera() const { return m_camera.value(); }
        Camera &camera() { return m_camera.value(); }

        // Procedural node graph management
        NodeGraph& nodeGraph() { return m_nodeGraph; }
        const NodeGraph& nodeGraph() const { return m_nodeGraph; }

        // Embedded texture management
        void addEmbeddedTexture(const std::string &id, EmbeddedTexture &&texture)
        {
            m_embeddedTextures.emplace(id, std::move(texture));
        }
        const EmbeddedTexture *getEmbeddedTexture(const std::string &id) const
        {
            auto it = m_embeddedTextures.find(id);
            return it != m_embeddedTextures.end() ? &it->second : nullptr;
        }
        bool hasEmbeddedTexture(const std::string &id) const
        {
            return m_embeddedTextures.find(id) != m_embeddedTextures.end();
        }
        const std::unordered_map<std::string, EmbeddedTexture> &embeddedTextures() const
        {
            return m_embeddedTextures;
        }

        // Environment map (HDR skybox) management
        void setEnvironmentMap(const std::string &path) { m_envMapPath = path; }
        const std::string &environmentMap() const { return m_envMapPath; }

        void setEnvironmentIntensity(float intensity) { m_envIntensity = intensity; }
        float environmentIntensity() const { return m_envIntensity; }

        void setEnvironmentRotation(float rotation) { m_envRotation = rotation; }
        float environmentRotation() const { return m_envRotation; }

    private:
        void addChildren(std::vector<SceneNode> &out, const Mat4 &parentTransform, size_t uid) const;

    private:
        std::vector<std::unique_ptr<Actor>> m_actors;
        std::unordered_map<std::string, std::unique_ptr<SceneObject>> m_objects;
        std::unordered_map<std::string, EmbeddedTexture> m_embeddedTextures;
        std::optional<Camera> m_camera;
        int64_t m_root = -1;
        NodeGraph m_nodeGraph{0, "SceneGraph"};  // Root node graph for the scene

        // Environment map (HDR skybox)
        std::string m_envMapPath;
        float m_envIntensity = 1.0f;
        float m_envRotation = 0.0f;
    };
}