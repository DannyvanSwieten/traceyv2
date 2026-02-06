#include "primitive_node.hpp"
#include "../../geometry.hpp"
#include "../node_registry.hpp"

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

    const InputsAndOutputs* PrimitiveNode::ports() const
    {
        static InputsAndOutputs portInfo;
        static bool initialized = false;

        if (!initialized) {
            // Primitives only output geometry (no inputs)
            portInfo.addOutput(PortInfo::createOutput("geometry", DataType::Geometry));
            initialized = true;
        }

        return &portInfo;
    }

    NodeEvaluationResult PrimitiveNode::evaluate(const EvaluationContext& ctx)
    {
        (void)ctx;
        NodeEvaluationResult result;

        try {
            std::shared_ptr<Geometry> geometry;

            switch (m_primitiveType) {
                case PrimitiveType::Cube: {
                    float size = 1.0f;
                    if (auto* param = getParameter("size")) {
                        if (auto* val = getValuePtr<float>(param->value())) {
                            size = *val;
                        }
                    }
                    geometry = std::make_shared<Geometry>(Geometry::createCube(size));
                    break;
                }

                case PrimitiveType::Sphere: {
                    float radius = 1.0f;
                    float segments = 16.0f;
                    float rings = 16.0f;
                    if (auto* p = getParameter("radius")) {
                        if (auto* v = getValuePtr<float>(p->value())) radius = *v;
                    }
                    if (auto* p = getParameter("segments")) {
                        if (auto* v = getValuePtr<float>(p->value())) segments = *v;
                    }
                    if (auto* p = getParameter("rings")) {
                        if (auto* v = getValuePtr<float>(p->value())) rings = *v;
                    }
                    geometry = std::make_shared<Geometry>(Geometry::createSphere(
                        radius, static_cast<uint32_t>(segments), static_cast<uint32_t>(rings)));
                    break;
                }

                case PrimitiveType::Torus: {
                    float majorRadius = 1.0f, minorRadius = 0.3f;
                    float majorSegments = 32.0f, minorSegments = 16.0f;
                    if (auto* p = getParameter("majorRadius")) {
                        if (auto* v = getValuePtr<float>(p->value())) majorRadius = *v;
                    }
                    if (auto* p = getParameter("minorRadius")) {
                        if (auto* v = getValuePtr<float>(p->value())) minorRadius = *v;
                    }
                    if (auto* p = getParameter("majorSegments")) {
                        if (auto* v = getValuePtr<float>(p->value())) majorSegments = *v;
                    }
                    if (auto* p = getParameter("minorSegments")) {
                        if (auto* v = getValuePtr<float>(p->value())) minorSegments = *v;
                    }
                    geometry = std::make_shared<Geometry>(Geometry::createTorus(
                        majorRadius, minorRadius,
                        static_cast<uint32_t>(majorSegments), static_cast<uint32_t>(minorSegments)));
                    break;
                }

                case PrimitiveType::Plane: {
                    float width = 1.0f, depth = 1.0f;
                    if (auto* p = getParameter("width")) {
                        if (auto* v = getValuePtr<float>(p->value())) width = *v;
                    }
                    if (auto* p = getParameter("depth")) {
                        if (auto* v = getValuePtr<float>(p->value())) depth = *v;
                    }
                    geometry = std::make_shared<Geometry>(Geometry::createPlane(width, depth));
                    break;
                }

                case PrimitiveType::Cylinder: {
                    float radius = 0.5f, height = 1.0f, segments = 32.0f;
                    if (auto* p = getParameter("radius")) {
                        if (auto* v = getValuePtr<float>(p->value())) radius = *v;
                    }
                    if (auto* p = getParameter("height")) {
                        if (auto* v = getValuePtr<float>(p->value())) height = *v;
                    }
                    if (auto* p = getParameter("segments")) {
                        if (auto* v = getValuePtr<float>(p->value())) segments = *v;
                    }
                    geometry = std::make_shared<Geometry>(Geometry::createCylinder(
                        radius, height, static_cast<uint32_t>(segments)));
                    break;
                }

                case PrimitiveType::Cone: {
                    float radius = 0.5f, height = 1.0f, segments = 32.0f;
                    if (auto* p = getParameter("radius")) {
                        if (auto* v = getValuePtr<float>(p->value())) radius = *v;
                    }
                    if (auto* p = getParameter("height")) {
                        if (auto* v = getValuePtr<float>(p->value())) height = *v;
                    }
                    if (auto* p = getParameter("segments")) {
                        if (auto* v = getValuePtr<float>(p->value())) segments = *v;
                    }
                    geometry = std::make_shared<Geometry>(Geometry::createCone(
                        radius, height, static_cast<uint32_t>(segments)));
                    break;
                }
            }

            if (geometry) {
                result.data = geometry;
                result.success = true;
            } else {
                result.success = false;
                result.error = "Failed to create primitive geometry";
            }

        } catch (const std::exception& e) {
            result.success = false;
            result.error = std::string("Exception in primitive evaluation: ") + e.what();
        }

        return result;
    }

    // ===== Static Node Registrations =====

    // CubeNode
    bool CubeNode::s_registered = CubeNode::registerNode();

    bool CubeNode::registerNode()
    {
        NodeDescriptor desc;
        desc.type = TRACEY_NODE_PRIMITIVE_CUBE;
        desc.name = "Cube";
        desc.description = "Cube primitive with controllable size";
        desc.category = NodeCategory::Primitive;
        desc.icon = "🔲";
        desc.factory = [](size_t uid, std::string name) -> std::unique_ptr<ProceduralNode> {
            return std::make_unique<CubeNode>(uid, std::move(name));
        };
        NodeRegistry::instance().registerNode(desc);
        return true;
    }

    // SphereNode
    bool SphereNode::s_registered = SphereNode::registerNode();

    bool SphereNode::registerNode()
    {
        NodeDescriptor desc;
        desc.type = TRACEY_NODE_PRIMITIVE_SPHERE;
        desc.name = "Sphere";
        desc.description = "Sphere primitive with radius and subdivision control";
        desc.category = NodeCategory::Primitive;
        desc.icon = "⚪";
        desc.factory = [](size_t uid, std::string name) -> std::unique_ptr<ProceduralNode> {
            return std::make_unique<SphereNode>(uid, std::move(name));
        };
        NodeRegistry::instance().registerNode(desc);
        return true;
    }

    // TorusNode
    bool TorusNode::s_registered = TorusNode::registerNode();

    bool TorusNode::registerNode()
    {
        NodeDescriptor desc;
        desc.type = TRACEY_NODE_PRIMITIVE_TORUS;
        desc.name = "Torus";
        desc.description = "Torus primitive with major/minor radius control";
        desc.category = NodeCategory::Primitive;
        desc.icon = "🍩";
        desc.factory = [](size_t uid, std::string name) -> std::unique_ptr<ProceduralNode> {
            return std::make_unique<TorusNode>(uid, std::move(name));
        };
        NodeRegistry::instance().registerNode(desc);
        return true;
    }

    // PlaneNode
    bool PlaneNode::s_registered = PlaneNode::registerNode();

    bool PlaneNode::registerNode()
    {
        NodeDescriptor desc;
        desc.type = TRACEY_NODE_PRIMITIVE_PLANE;
        desc.name = "Plane";
        desc.description = "Flat plane primitive with width and depth";
        desc.category = NodeCategory::Primitive;
        desc.icon = "▭";
        desc.factory = [](size_t uid, std::string name) -> std::unique_ptr<ProceduralNode> {
            return std::make_unique<PlaneNode>(uid, std::move(name));
        };
        NodeRegistry::instance().registerNode(desc);
        return true;
    }

    // CylinderNode
    bool CylinderNode::s_registered = CylinderNode::registerNode();

    bool CylinderNode::registerNode()
    {
        NodeDescriptor desc;
        desc.type = TRACEY_NODE_PRIMITIVE_CYLINDER;
        desc.name = "Cylinder";
        desc.description = "Cylinder primitive with radius and height";
        desc.category = NodeCategory::Primitive;
        desc.icon = "🛢";
        desc.factory = [](size_t uid, std::string name) -> std::unique_ptr<ProceduralNode> {
            return std::make_unique<CylinderNode>(uid, std::move(name));
        };
        NodeRegistry::instance().registerNode(desc);
        return true;
    }

    // ConeNode
    bool ConeNode::s_registered = ConeNode::registerNode();

    bool ConeNode::registerNode()
    {
        NodeDescriptor desc;
        desc.type = TRACEY_NODE_PRIMITIVE_CONE;
        desc.name = "Cone";
        desc.description = "Cone primitive with base radius and height";
        desc.category = NodeCategory::Primitive;
        desc.icon = "🔺";
        desc.factory = [](size_t uid, std::string name) -> std::unique_ptr<ProceduralNode> {
            return std::make_unique<ConeNode>(uid, std::move(name));
        };
        NodeRegistry::instance().registerNode(desc);
        return true;
    }

} // namespace tracey
