#include "scene.hpp"

namespace tracey
{
    Scene::Scene()
    {
        // Automatically create root actor
        m_actors.emplace_back(std::make_unique<Actor>(this, 0));
        m_actors[0]->setName("Root");
        m_root = 0;
    }

    Actor *Scene::createActor()
    {
        // Create actor and parent it to root
        return createActorUnderParent(static_cast<size_t>(m_root));
    }

    Actor *Scene::createActorUnderParent(size_t parentUid)
    {
        m_actors.emplace_back(std::make_unique<Actor>(this, m_actors.size()));
        Actor *newActor = m_actors.back().get();

        // Parent to specified actor (or root if invalid)
        Actor *parent = getActor(parentUid);
        if (parent)
        {
            parent->addChild(newActor);
        }
        else if (m_root >= 0)
        {
            // Fallback to root if invalid parent specified
            getRoot()->addChild(newActor);
        }

        return newActor;
    }

    Actor *Scene::createActorWithUid(size_t uid)
    {
        // Ensure vector is large enough
        if (uid >= m_actors.size())
        {
            m_actors.resize(uid + 1);
        }

        // Check if actor already exists at this slot
        if (m_actors[uid])
        {
            return m_actors[uid].get();  // Return existing actor
        }

        // Create new actor at this specific UID
        m_actors[uid] = std::make_unique<Actor>(this, uid);
        Actor *newActor = m_actors[uid].get();

        // Parent to root (matching createActor behavior)
        if (m_root >= 0)
        {
            getRoot()->addChild(newActor);
        }

        return newActor;
    }

    void Scene::removeActor(size_t uid)
    {
        if (uid >= m_actors.size())
            return;

        // Don't allow removing the root
        if (static_cast<int64_t>(uid) == m_root)
            return;

        auto *actor = m_actors[uid].get();
        if (!actor)
            return;  // Already removed

        // Recursively remove all children first
        // Make a copy of children list since we'll be modifying it
        auto childrenSpan = actor->children();
        std::vector<size_t> childrenCopy(childrenSpan.begin(), childrenSpan.end());
        for (size_t childUid : childrenCopy)
        {
            removeActor(childUid);  // Recursive call
        }

        // Remove from parent's children list using parent tracking (O(1) lookup)
        if (actor->hasParent())
        {
            Actor *parent = getActor(actor->parent());
            if (parent)
            {
                parent->removeChild(uid);
            }
        }

        m_actors[uid].reset();
    }

    void Scene::clear()
    {
        m_actors.clear();
        m_objects.clear();
        m_embeddedTextures.clear();
        m_camera.reset();

        // Recreate root actor
        m_actors.emplace_back(std::make_unique<Actor>(this, 0));
        m_actors[0]->setName("Root");
        m_root = 0;
    }
    std::vector<SceneNode> Scene::flatten() const
    {
        std::vector<SceneNode> out;
        // Only traverse from top-level actors (those without parents)
        // Using the hasParent() method is O(1) instead of O(n²) child scan
        for (const auto &actorPtr : m_actors)
        {
            if (actorPtr && !actorPtr->hasParent())
            {
                addChildren(out, Mat4(1.0), actorPtr->getUid());
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