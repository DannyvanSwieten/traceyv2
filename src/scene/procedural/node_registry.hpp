#pragma once

#include "node.hpp"
#include "../../c_api/tracey_api.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tracey
{
    /**
     * @brief Category for grouping nodes in UI
     */
    enum class NodeCategory
    {
        Actor,      // ActorNode - containers with geometry networks
        Primitive,  // Geometric primitives (cube, sphere, etc.)
        Geometry,   // Geometry operations (transform, merge, etc.)
        Math,       // Mathematical operations
        Utility     // Utility nodes
    };

    /**
     * @brief Metadata descriptor for a node type
     *
     * Contains all the information needed to create and display nodes in the UI
     */
    struct NodeDescriptor
    {
        TraceyNodeType type;             // C API node type enum
        std::string name;                // Display name (e.g., "Cube Primitive")
        std::string description;         // User-facing description
        NodeCategory category;           // Category for UI grouping
        std::string icon;                // Emoji or icon identifier

        // Factory function pointer - creates a new node instance
        using FactoryFn = std::function<std::unique_ptr<ProceduralNode>(size_t uid, std::string name)>;
        FactoryFn factory;
    };

    /**
     * @brief Singleton registry for node types
     *
     * Replaces the hardcoded switch-statement factory pattern with a metadata-driven
     * registration system. Each node type registers itself with metadata on startup.
     */
    class NodeRegistry
    {
    public:
        // Get singleton instance
        static NodeRegistry& instance();

        // Registration API - called by node types during static initialization
        void registerNode(const NodeDescriptor& descriptor);

        // Factory API - creates nodes using registered factory functions
        std::unique_ptr<ProceduralNode> createNode(
            TraceyNodeType type,
            size_t uid,
            const std::string& name
        );

        // Query API - retrieve node metadata
        const NodeDescriptor* getDescriptor(TraceyNodeType type) const;
        std::vector<NodeDescriptor> getNodesByCategory(NodeCategory category) const;
        std::vector<NodeDescriptor> getAllNodes() const;
        bool hasNodeType(TraceyNodeType type) const;

        // Force registration of all nodes (call at startup)
        static void ensureAllNodesRegistered();

    private:
        NodeRegistry() = default;
        NodeRegistry(const NodeRegistry&) = delete;
        NodeRegistry& operator=(const NodeRegistry&) = delete;

        // Map from node type to descriptor
        std::unordered_map<TraceyNodeType, NodeDescriptor> m_descriptors;
    };

} // namespace tracey
