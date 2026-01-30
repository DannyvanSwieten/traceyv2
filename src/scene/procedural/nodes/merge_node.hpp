#pragma once

#include "../node.hpp"
#include "../../geometry.hpp"

namespace tracey
{
    /**
     * @brief Merge node - combines multiple geometries into one
     *
     * Takes multiple geometry inputs and merges them into a single SceneObject.
     * This is a fundamental SOP (Surface Operator) node.
     *
     * Inputs: Multiple geometry connections (via node connections)
     * Output: Single merged SceneObject
     */
    class MergeNode : public ProceduralNode
    {
    public:
        MergeNode(size_t uid, std::string name);
        virtual ~MergeNode() = default;

        // Node evaluation
        NodeEvaluationResult evaluate(const EvaluationContext& ctx) override;

    private:
        void initializeParameters();

        // Helper to merge geometry b into a (modifies a in place)
        static void mergeGeometry(Geometry& a, const Geometry& b);
    };

} // namespace tracey
