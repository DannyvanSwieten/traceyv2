#pragma once

#include "../node.hpp"
#include "../node_graph.hpp"
#include "../../transform.hpp"
#include <vector>

namespace tracey
{
    // Forward declaration
    class Actor;

    /**
     * @brief Actor node - container with geometry network and transform
     *
     * ActorNode is a special hierarchical node that:
     * - Contains a nested NodeGraph for geometry operations
     * - Stores child actor references (hierarchical scene graph)
     * - Has transform parameters (position, rotation, scale)
     * - Optionally wraps an existing Actor for backward compatibility
     *
     * Phase 1: Basic structure with transform parameters
     * Phase 2: Geometry network evaluation
     */
    class ActorNode : public ProceduralNode
    {
    public:
        ActorNode(size_t uid, std::string name);
        virtual ~ActorNode() = default;

        // Nested geometry network
        NodeGraph& geometryNetwork() { return m_geometryNetwork; }
        const NodeGraph& geometryNetwork() const { return m_geometryNetwork; }

        // Child actor management (hierarchical scene graph)
        void addChild(size_t childActorUid);
        void removeChild(size_t childActorUid);
        const std::vector<size_t>& children() const { return m_children; }

        // Transform parameter accessors (convenience methods)
        Parameter* positionParam() { return getParameter("position"); }
        Parameter* rotationParam() { return getParameter("rotation"); }
        Parameter* scaleParam() { return getParameter("scale"); }

        const Parameter* positionParam() const { return getParameter("position"); }
        const Parameter* rotationParam() const { return getParameter("rotation"); }
        const Parameter* scaleParam() const { return getParameter("scale"); }

        // Get computed transform from parameters
        Transform getTransform() const;
        void setTransform(const Transform& transform);

        // Material assignment (Phase 2)
        // Can be connected to MaterialNode output
        void setMaterialNodeUid(size_t materialNodeUid) { m_materialNodeUid = materialNodeUid; }
        size_t materialNodeUid() const { return m_materialNodeUid; }

        // Backward compatibility: wrap existing Actor
        // This allows ActorNode to work with the existing scene system
        void setWrappedActor(Actor* actor) { m_wrappedActor = actor; }
        Actor* wrappedActor() const { return m_wrappedActor; }

        // Node evaluation (Phase 2)
        // For Phase 1, returns empty result
        NodeEvaluationResult evaluate(const EvaluationContext& ctx) override;

    private:
        NodeGraph m_geometryNetwork;           // Internal geometry operations
        std::vector<size_t> m_children;        // Child actor UIDs
        size_t m_materialNodeUid = 0;          // Material node connection (0 = none)
        Actor* m_wrappedActor = nullptr;       // Optional: wrapped existing Actor

        void initializeParameters();
    };

} // namespace tracey
