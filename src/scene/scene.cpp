#include "scene.hpp"

namespace tracey
{
    Actor *Scene::createActor()
    {
        if (m_root == -1)
            m_root = 0;

        m_actors.emplace_back(std::make_unique<Actor>(this, m_actors.size()));
        return m_actors.back().get();
    }
    void Scene::removeActor(size_t uid)
    {
        if (uid >= m_actors.size())
            return;

        // Remove from parent's children list if applicable
        for (auto &actorPtr : m_actors)
        {
            actorPtr->removeChild(uid);
        }

        m_actors[uid].reset();
    }
    std::vector<SceneNode> Scene::flatten() const
    {
        std::vector<SceneNode> out;
        if (m_root != -1)
        {
            addChildren(out, Mat4(1.0), m_root);
        }
        return out;
    }
    void Scene::addChildren(std::vector<SceneNode> &out, const Mat4 &parentTransform, size_t uid) const
    {
        const Actor *actor = getActor(uid);
        if (!actor)
            return;

        Mat4 worldTransform = parentTransform * actor->transform().toMatrix();
        out.push_back({worldTransform, const_cast<Actor *>(actor)});
        for (auto childUid : actor->children())
        {
            addChildren(out, worldTransform, childUid);
        }
    }
} // namespace tracey