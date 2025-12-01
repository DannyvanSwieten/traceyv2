#pragma once
#include <vector>
#include "actor.hpp"

namespace tracey
{
    struct SceneNode
    {
        Mat4 worldTransform;
        const Actor *const actor;
    };

    class Scene
    {
    public:
        Scene() = default;
        ~Scene() = default;

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
        std::vector<SceneNode> flatten() const;

    private:
        void addChildren(std::vector<SceneNode> &out, const Mat4 &parentTransform, size_t uid) const;

    private:
        std::vector<std::unique_ptr<Actor>> m_actors;
        size_t m_root = -1;
    };
}