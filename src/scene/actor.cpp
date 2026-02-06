#include "actor.hpp"
#include "scene.hpp"
namespace tracey
{
    void Actor::setTransform(const Transform &transform)
    {
        m_transform = transform;
    }

    void Actor::applyTransform(const Transform &deltaTransform)
    {
        m_transform.applyRotation(deltaTransform.rotation());
        m_transform.applyScale(deltaTransform.scale());
        m_transform.setPosition(m_transform.position() + deltaTransform.position());
    }

    const Transform &Actor::transform() const
    {
        return m_transform;
    }
    const std::span<const size_t> Actor::children() const
    {
        return m_children;
    }
    void Actor::removeChild(size_t childUid)
    {
        m_children.erase(std::remove(m_children.begin(), m_children.end(), childUid), m_children.end());
    }

    void Actor::addChild(Actor *child)
    {
        // If child already has a parent, remove it from the old parent's children list
        if (child->hasParent() && m_scene)
        {
            Actor *oldParent = m_scene->getActor(child->parent());
            if (oldParent)
            {
                oldParent->removeChild(child->uid);
            }
        }

        // Add to this actor's children list
        m_children.push_back(child->uid);
        child->m_parent = uid;
    }
}