#pragma once

#include "../node.hpp"
#include "../../geometry.hpp"

namespace tracey
{
    /**
     * @brief Primitive type enumeration
     */
    enum class PrimitiveType
    {
        Cube,
        Sphere,
        Torus,
        Plane,
        Cylinder,
        Cone
    };

    /**
     * @brief Geometry primitive node (cube, sphere, etc.)
     *
     * Creates basic geometric primitives with controllable parameters.
     * Each primitive type has its own specific parameters:
     * - Cube: size
     * - Sphere: radius, segments, rings
     * - Torus: majorRadius, minorRadius, majorSegments, minorSegments
     * - Plane: width, depth
     * - Cylinder: radius, height, segments
     * - Cone: radius, height, segments
     *
     * Phase 1: Parameter definitions
     * Phase 2: Actual geometry generation
     */
    class PrimitiveNode : public ProceduralNode
    {
    public:
        PrimitiveNode(size_t uid, std::string name, PrimitiveType primitiveType);
        virtual ~PrimitiveNode() = default;

        PrimitiveType primitiveType() const { return m_primitiveType; }

        // Node evaluation (Phase 2)
        // For Phase 1, returns empty result
        NodeEvaluationResult evaluate(const EvaluationContext& ctx) override;

    private:
        PrimitiveType m_primitiveType;

        void initializeParametersForType();
    };

    /**
     * @brief Specific primitive node types
     */
    class CubeNode : public PrimitiveNode
    {
    public:
        CubeNode(size_t uid, std::string name)
            : PrimitiveNode(uid, std::move(name), PrimitiveType::Cube) {}
    };

    class SphereNode : public PrimitiveNode
    {
    public:
        SphereNode(size_t uid, std::string name)
            : PrimitiveNode(uid, std::move(name), PrimitiveType::Sphere) {}
    };

    class TorusNode : public PrimitiveNode
    {
    public:
        TorusNode(size_t uid, std::string name)
            : PrimitiveNode(uid, std::move(name), PrimitiveType::Torus) {}
    };

    class PlaneNode : public PrimitiveNode
    {
    public:
        PlaneNode(size_t uid, std::string name)
            : PrimitiveNode(uid, std::move(name), PrimitiveType::Plane) {}
    };

    class CylinderNode : public PrimitiveNode
    {
    public:
        CylinderNode(size_t uid, std::string name)
            : PrimitiveNode(uid, std::move(name), PrimitiveType::Cylinder) {}
    };

    class ConeNode : public PrimitiveNode
    {
    public:
        ConeNode(size_t uid, std::string name)
            : PrimitiveNode(uid, std::move(name), PrimitiveType::Cone) {}
    };

} // namespace tracey
