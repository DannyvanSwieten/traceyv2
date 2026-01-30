#include "primitive_node.hpp"

namespace tracey
{
    PrimitiveNode::PrimitiveNode(size_t uid, std::string name, PrimitiveType primitiveType)
        : ProceduralNode(uid, NodeType::GeometryPrimitive, std::move(name))
        , m_primitiveType(primitiveType)
    {
        initializeParametersForType();
    }

    void PrimitiveNode::initializeParametersForType()
    {
        switch (m_primitiveType) {
            case PrimitiveType::Cube:
                // Cube: single size parameter
                addFloatParameter("size", 1.0f, 0.01f, 100.0f);
                break;

            case PrimitiveType::Sphere:
                // Sphere: radius, segments, rings
                addFloatParameter("radius", 1.0f, 0.01f, 100.0f);
                addFloatParameter("segments", 16.0f, 3.0f, 128.0f);
                addFloatParameter("rings", 16.0f, 3.0f, 128.0f);
                break;

            case PrimitiveType::Torus:
                // Torus: major/minor radius, segments
                addFloatParameter("majorRadius", 1.0f, 0.01f, 100.0f);
                addFloatParameter("minorRadius", 0.3f, 0.01f, 100.0f);
                addFloatParameter("majorSegments", 32.0f, 3.0f, 128.0f);
                addFloatParameter("minorSegments", 16.0f, 3.0f, 128.0f);
                break;

            case PrimitiveType::Plane:
                // Plane: width, depth
                addFloatParameter("width", 1.0f, 0.01f, 100.0f);
                addFloatParameter("depth", 1.0f, 0.01f, 100.0f);
                break;

            case PrimitiveType::Cylinder:
                // Cylinder: radius, height, segments
                addFloatParameter("radius", 0.5f, 0.01f, 100.0f);
                addFloatParameter("height", 1.0f, 0.01f, 100.0f);
                addFloatParameter("segments", 32.0f, 3.0f, 128.0f);
                break;

            case PrimitiveType::Cone:
                // Cone: radius, height, segments
                addFloatParameter("radius", 0.5f, 0.01f, 100.0f);
                addFloatParameter("height", 1.0f, 0.01f, 100.0f);
                addFloatParameter("segments", 32.0f, 3.0f, 128.0f);
                break;
        }

        // Mark all parameters as animatable
        for (auto& [name, param] : parameters()) {
            param->setFlags(static_cast<uint32_t>(ParameterFlags::Animatable));
        }
    }

    NodeEvaluationResult PrimitiveNode::evaluate(const EvaluationContext& ctx)
    {
        // Phase 2: Actual geometry generation
        // For Phase 1, return empty result
        (void)ctx;

        // TODO: Phase 2
        // 1. Get parameter values
        // 2. Create SceneObject based on primitive type and parameters
        // 3. Return result with geometry

        return NodeEvaluationResult();
    }

} // namespace tracey
