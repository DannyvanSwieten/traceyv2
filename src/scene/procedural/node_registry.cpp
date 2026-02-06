#include "node_registry.hpp"
#include "nodes/actor_node.hpp"
#include "nodes/primitive_node.hpp"
#include "nodes/transform_geo_node.hpp"
#include "nodes/merge_node.hpp"
#include <stdexcept>
#include <iostream>

namespace tracey
{
    NodeRegistry& NodeRegistry::instance()
    {
        static NodeRegistry registry;
        return registry;
    }

    void NodeRegistry::registerNode(const NodeDescriptor& descriptor)
    {
        // Check if already registered
        if (m_descriptors.find(descriptor.type) != m_descriptors.end()) {
            throw std::runtime_error(
                "Node type already registered: " + descriptor.name +
                " (type: " + std::to_string(static_cast<int>(descriptor.type)) + ")"
            );
        }

        // Register the descriptor
        m_descriptors[descriptor.type] = descriptor;

        // Debug logging
        std::cout << "NodeRegistry: Registered '" << descriptor.name
                  << "' (type: " << static_cast<int>(descriptor.type)
                  << ", category: " << static_cast<int>(descriptor.category)
                  << ")" << std::endl;
    }

    std::unique_ptr<ProceduralNode> NodeRegistry::createNode(
        TraceyNodeType type,
        size_t uid,
        const std::string& name
    )
    {
        auto it = m_descriptors.find(type);
        if (it == m_descriptors.end()) {
            throw std::runtime_error(
                "Unknown node type: " + std::to_string(static_cast<int>(type))
            );
        }

        // Call the factory function
        return it->second.factory(uid, name);
    }

    const NodeDescriptor* NodeRegistry::getDescriptor(TraceyNodeType type) const
    {
        auto it = m_descriptors.find(type);
        return (it != m_descriptors.end()) ? &it->second : nullptr;
    }

    std::vector<NodeDescriptor> NodeRegistry::getNodesByCategory(NodeCategory category) const
    {
        std::vector<NodeDescriptor> result;
        for (const auto& [type, desc] : m_descriptors) {
            if (desc.category == category) {
                result.push_back(desc);
            }
        }
        return result;
    }

    std::vector<NodeDescriptor> NodeRegistry::getAllNodes() const
    {
        std::vector<NodeDescriptor> result;
        result.reserve(m_descriptors.size());
        for (const auto& [type, desc] : m_descriptors) {
            result.push_back(desc);
        }
        return result;
    }

    bool NodeRegistry::hasNodeType(TraceyNodeType type) const
    {
        return m_descriptors.find(type) != m_descriptors.end();
    }

    void NodeRegistry::ensureAllNodesRegistered()
    {
        // Force linking of all node types by referencing their static registration flags
        // This ensures the linker includes the object files and runs static initializers
        volatile bool forceLink =
            ActorNode::s_registered &&
            CubeNode::s_registered &&
            SphereNode::s_registered &&
            TorusNode::s_registered &&
            PlaneNode::s_registered &&
            CylinderNode::s_registered &&
            ConeNode::s_registered &&
            TransformGeoNode::s_registered &&
            MergeNode::s_registered;

        (void)forceLink;  // Prevent unused variable warning

        std::cout << "NodeRegistry: " << instance().m_descriptors.size()
                  << " node types registered" << std::endl;
    }

} // namespace tracey
