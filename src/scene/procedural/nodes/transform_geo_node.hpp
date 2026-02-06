#pragma once

#include "../node.hpp"
#include "../../geometry.hpp"

namespace tracey
{
    /**
     * @brief Transform geometry node - applies spatial transformations to geometry
     *
     * This is a fundamental SOP (Surface Operator) node that transforms all points
     * in the input geometry by the specified translation, rotation, and scale.
     *
     * Parameters:
     * - translate (Vec3): Translation offset
     * - rotate (Vec3): Rotation in degrees (euler angles XYZ)
     * - scale (Vec3): Scale factors per axis
     * - uniformScale (float): Uniform scale multiplier
     *
     * The transformation order is: Scale -> Rotate -> Translate
     */
    class TransformGeoNode : public ProceduralNode
    {
    public:
        TransformGeoNode(size_t uid, std::string name);
        virtual ~TransformGeoNode() = default;

        // Node evaluation
        NodeEvaluationResult evaluate(const EvaluationContext& ctx) override;

        // Port information (Phase 2)
        const InputsAndOutputs* ports() const override;

        // Static node registry registration
        static bool registerNode();
        static bool s_registered;  // Static registration flag

    private:
        void initializeParameters();
    };

} // namespace tracey
