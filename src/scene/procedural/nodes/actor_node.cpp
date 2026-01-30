#include "actor_node.hpp"
#include "../../actor.hpp"
#include <algorithm>

namespace tracey
{
    ActorNode::ActorNode(size_t uid, std::string name)
        : ProceduralNode(uid, NodeType::Actor, std::move(name))
        , m_geometryNetwork(uid * 1000, "GeometryNetwork")  // Generate unique UID for nested graph
    {
        initializeParameters();
    }

    void ActorNode::initializeParameters()
    {
        // Transform parameters
        // These can be keyframed (Phase 4) or connected (Phase 3)
        addVec3Parameter("position", Vec3(0.0f, 0.0f, 0.0f));
        addVec3Parameter("rotation", Vec3(0.0f, 0.0f, 0.0f));  // Euler angles
        addVec3Parameter("scale", Vec3(1.0f, 1.0f, 1.0f));

        // Set flags
        if (auto* pos = getParameter("position")) {
            pos->setFlags(
                static_cast<uint32_t>(ParameterFlags::Animatable) |
                static_cast<uint32_t>(ParameterFlags::Connectable) |
                static_cast<uint32_t>(ParameterFlags::Exposable)
            );
        }
        if (auto* rot = getParameter("rotation")) {
            rot->setFlags(
                static_cast<uint32_t>(ParameterFlags::Animatable) |
                static_cast<uint32_t>(ParameterFlags::Connectable) |
                static_cast<uint32_t>(ParameterFlags::Exposable)
            );
        }
        if (auto* scale = getParameter("scale")) {
            scale->setFlags(
                static_cast<uint32_t>(ParameterFlags::Animatable) |
                static_cast<uint32_t>(ParameterFlags::Connectable) |
                static_cast<uint32_t>(ParameterFlags::Exposable)
            );
        }
    }

    void ActorNode::addChild(size_t childActorUid)
    {
        // Add child if not already present
        if (std::find(m_children.begin(), m_children.end(), childActorUid) == m_children.end()) {
            m_children.push_back(childActorUid);
        }
    }

    void ActorNode::removeChild(size_t childActorUid)
    {
        m_children.erase(
            std::remove(m_children.begin(), m_children.end(), childActorUid),
            m_children.end()
        );
    }

    Transform ActorNode::getTransform() const
    {
        Transform transform;

        // Get position from parameter
        if (const auto* posParam = positionParam()) {
            if (const auto* pos = getValuePtr<Vec3>(posParam->value())) {
                transform.setPosition(*pos);
            }
        }

        // Get rotation from parameter (Euler angles -> Quaternion)
        if (const auto* rotParam = rotationParam()) {
            if (const auto* rot = getValuePtr<Vec3>(rotParam->value())) {
                // Convert Euler angles (degrees) to quaternion
                transform.setRotation(Quaternion(glm::radians(*rot)));
            }
        }

        // Get scale from parameter
        if (const auto* scaleParam = this->scaleParam()) {
            if (const auto* scale = getValuePtr<Vec3>(scaleParam->value())) {
                transform.setScale(*scale);
            }
        }

        return transform;
    }

    void ActorNode::setTransform(const Transform& transform)
    {
        // Set position parameter
        if (auto* pos = getParameter("position")) {
            pos->setValue(transform.position());
        }

        // Set rotation parameter (Quaternion -> Euler angles)
        if (auto* rot = getParameter("rotation")) {
            Vec3 euler = glm::degrees(glm::eulerAngles(transform.rotation()));
            rot->setValue(euler);
        }

        // Set scale parameter
        if (auto* scale = getParameter("scale")) {
            scale->setValue(transform.scale());
        }

        setDirty(true);
    }

    NodeEvaluationResult ActorNode::evaluate(const EvaluationContext& ctx)
    {
        // Phase 2: Full evaluation implementation
        // For Phase 1, return empty result
        (void)ctx;

        // TODO: Phase 2
        // 1. Evaluate geometry network
        // 2. Apply transform to geometry
        // 3. Apply material (if connected)
        // 4. Return result as SceneInstance(s)

        return NodeEvaluationResult();
    }

} // namespace tracey
