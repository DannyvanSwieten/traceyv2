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

    void Scene::clear()
    {
        m_actors.clear();
        m_objects.clear();
        m_embeddedTextures.clear();
        m_camera.reset();
        m_root = -1;
    }
    std::vector<SceneNode> Scene::flatten() const
    {
        std::vector<SceneNode> out;
        // Iterate over all actors directly
        // This handles both hierarchical scenes (via children) and flat lists
        for (const auto &actorPtr : m_actors)
        {
            if (actorPtr)
            {
                // Check if this actor has a parent - skip if it does (it will be visited via parent)
                bool hasParent = false;
                for (const auto &otherPtr : m_actors)
                {
                    if (otherPtr && otherPtr.get() != actorPtr.get())
                    {
                        for (auto childUid : otherPtr->children())
                        {
                            if (childUid == actorPtr->getUid())
                            {
                                hasParent = true;
                                break;
                            }
                        }
                    }
                    if (hasParent)
                        break;
                }

                // Only add top-level actors (those without parents)
                if (!hasParent)
                {
                    addChildren(out, Mat4(1.0), actorPtr->getUid());
                }
            }
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

    void Scene::addObject(const std::string &name, std::unique_ptr<SceneObject> obj)
    {
        obj->setName(name);
        m_objects[name] = std::move(obj);
    }

    void Scene::addObject(const std::string &name, SceneObject &&obj)
    {
        obj.setName(name);
        m_objects[name] = std::make_unique<SceneObject>(std::move(obj));
    }

    SceneObject *Scene::getObject(const std::string &name)
    {
        auto it = m_objects.find(name);
        if (it != m_objects.end())
        {
            return it->second.get();
        }
        return nullptr;
    }

    const SceneObject *Scene::getObject(const std::string &name) const
    {
        auto it = m_objects.find(name);
        if (it != m_objects.end())
        {
            return it->second.get();
        }
        return nullptr;
    }

    bool Scene::hasObject(const std::string &name) const
    {
        return m_objects.find(name) != m_objects.end();
    }
} // namespace tracey