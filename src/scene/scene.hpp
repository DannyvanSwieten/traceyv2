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
        Scene() = default;
        ~Scene() = default;

        // Actor management
        Actor *createActor();
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

        // Live actors only. The underlying m_actors is sparse — removeActor
        // resets a slot to null rather than erasing so existing uids stay
        // valid as indices via getActor(uid) — and every caller that just
        // wants to iterate "all actors" needs the non-sparse view.
        //
        // Returning a fresh filtered vector each call is cheap (pointers
        // only) and makes the null check at every consumer impossible to
        // forget, which had been a recurring source of crashes in
        // save_scene_to_file / get_all_actors / etc.
        //
        // For uid-keyed indexing into the sparse table, use getActor(uid).
        std::vector<Actor *> actors()
        {
            std::vector<Actor *> out;
            out.reserve(m_actors.size());
            for (const auto &a : m_actors) if (a) out.push_back(a.get());
            return out;
        }
        std::vector<const Actor *> actors() const
        {
            std::vector<const Actor *> out;
            out.reserve(m_actors.size());
            for (const auto &a : m_actors) if (a) out.push_back(a.get());
            return out;
        }

        // Object management
        void addObject(const std::string &name, std::unique_ptr<SceneObject> obj);
        void addObject(const std::string &name, SceneObject &&obj);
        SceneObject *getObject(const std::string &name);
        const SceneObject *getObject(const std::string &name) const;
        bool hasObject(const std::string &name) const;
        // Drop a SceneObject by name. No-op if the name isn't in the table.
        // Callers must ensure no live Actor's SceneInstance still references
        // the removed name — the SceneCompiler skips missing refs cleanly
        // (line ~454 in scene_compiler.cpp), but the rasterizer + path
        // tracer will silently drop instances of orphaned objects, so
        // typical use is "remove the object as part of removing its actor".
        void removeObject(const std::string &name);
        const std::unordered_map<std::string, std::unique_ptr<SceneObject>> &objects() const { return m_objects; }

        // Camera management
        void setCamera(const Camera &camera) { m_camera = camera; }
        bool hasCamera() const { return m_camera.has_value(); }
        const Camera &camera() const { return m_camera.value(); }
        Camera &camera() { return m_camera.value(); }

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

    private:
        void addChildren(std::vector<SceneNode> &out, const Mat4 &parentTransform, size_t uid) const;

    private:
        std::vector<std::unique_ptr<Actor>> m_actors;
        std::unordered_map<std::string, std::unique_ptr<SceneObject>> m_objects;
        std::unordered_map<std::string, EmbeddedTexture> m_embeddedTextures;
        std::optional<Camera> m_camera;
        int64_t m_root = -1;
    };
}