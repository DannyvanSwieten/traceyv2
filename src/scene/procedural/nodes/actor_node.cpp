#include "actor_node.hpp"
#include "../../actor.hpp"
#include "../../geometry.hpp"
#include "../evaluation_context.hpp"
#include "../node_registry.hpp"
#include <algorithm>

namespace tracey
{
    // Static registration (called before main())
    bool ActorNode::s_registered = ActorNode::registerNode();

    bool ActorNode::registerNode()
    {
        NodeDescriptor desc;
        desc.type = TRACEY_NODE_ACTOR;
        desc.name = "Actor";
        desc.description = "Container node that holds geometry and child actors";
        desc.category = NodeCategory::Actor;
        desc.icon = "🎬";
        desc.factory = [](size_t uid, std::string name) -> std::unique_ptr<ProceduralNode> {
            return std::make_unique<ActorNode>(uid, std::move(name));
        };

        NodeRegistry::instance().registerNode(desc);
        return true;
    }

    ActorNode::ActorNode(size_t uid, std::string name)
        : ProceduralNode(uid, NodeType::Actor, std::move(name))
        , m_geometryNetwork(uid * 1000, "GeometryNetwork")  // Generate unique UID for nested graph
    {
        // Set this ActorNode as the owner of the nested graph
        m_geometryNetwork.setOwnerNodeUid(uid);

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

    const InputsAndOutputs* ActorNode::ports() const
    {
        static InputsAndOutputs portInfo;
        static bool initialized = false;

        if (!initialized) {
            portInfo.addInput(PortInfo::createInput("geometry", DataType::Geometry));
            portInfo.addOutput(PortInfo::createOutput("geometry", DataType::Geometry));
            initialized = true;
        }

        return &portInfo;
    }

    NodeEvaluationResult ActorNode::evaluate(const EvaluationContext& ctx)
    {
        try {
            // Step 1: Mark geometry network nodes dirty and evaluate
            // This ensures nodes are re-evaluated even if they were marked clean previously
            m_geometryNetwork.markDirty();

            EvaluationContext nestedCtx = ctx;
            nestedCtx.graph = &m_geometryNetwork;
            nestedCtx.cache.clear(); // Separate cache for nested evaluation

            GraphEvaluationResult graphResult = m_geometryNetwork.evaluate(nestedCtx);

            if (!graphResult.success) {
                return NodeEvaluationResult::makeError(
                    "ActorNode '" + name() + "' geometry network failed: " + graphResult.error
                );
            }

            // Step 2: Get "geometry" output from nested graph
            auto outputIt = graphResult.outputs.find("geometry");
            if (outputIt == graphResult.outputs.end()) {
                // No geometry output - return empty (valid for container-only ActorNodes)
                return NodeEvaluationResult();
            }

            const NodeEvaluationResult& outputResult = outputIt->second;

            // Step 3: Extract geometry from output node result
            auto* geometryPtr = std::get_if<std::shared_ptr<Geometry>>(&outputResult.data);
            if (!geometryPtr || !*geometryPtr) {
                return NodeEvaluationResult::makeError(
                    "ActorNode '" + name() + "' output is not geometry"
                );
            }

            // Step 4: Return geometry WITHOUT baking transform
            // The transform is applied at the Actor/TLAS level during scene compilation
            // This avoids double-application of transforms
            return NodeEvaluationResult(*geometryPtr);

        } catch (const std::exception& e) {
            return NodeEvaluationResult::makeError(
                std::string("ActorNode evaluation exception: ") + e.what()
            );
        }
    }

    std::shared_ptr<Geometry> ActorNode::applyTransformToGeometry(const std::shared_ptr<Geometry>& inputGeometry)
    {
        // Get transform from parameters
        Transform transform = getTransform();
        Mat4 matrix = transform.toMatrix();

        // Create new geometry with transformed positions
        auto outputGeometry = std::make_shared<Geometry>();

        // Transform positions
        std::vector<Vec3> transformedPositions;
        transformedPositions.reserve(inputGeometry->positions().size());
        for (const Vec3& pos : inputGeometry->positions()) {
            Vec4 transformed = matrix * Vec4(pos, 1.0f);
            transformedPositions.push_back(Vec3(transformed));
        }
        outputGeometry->setPositions(std::move(transformedPositions));

        // Transform normals (use inverse transpose for normals)
        if (inputGeometry->hasNormals()) {
            Mat3 normalMatrix = glm::transpose(glm::inverse(Mat3(matrix)));
            std::vector<Vec3> transformedNormals;
            transformedNormals.reserve(inputGeometry->normals().size());
            for (const Vec3& normal : inputGeometry->normals()) {
                Vec3 transformed = glm::normalize(normalMatrix * normal);
                transformedNormals.push_back(transformed);
            }
            outputGeometry->setNormals(std::move(transformedNormals));
        }

        // Copy indices and UVs unchanged
        outputGeometry->setIndices(inputGeometry->indices());
        if (inputGeometry->hasUvs()) {
            outputGeometry->setUvs(inputGeometry->uvs());
        }

        return outputGeometry;
    }

} // namespace tracey
