#pragma once

#include "../../graph/graph.hpp"
#include "node.hpp"
#include "evaluation_context.hpp"
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

namespace tracey
{
    /**
     * @brief Connection between two nodes
     */
    struct NodeConnection
    {
        size_t fromNode;         // Source node UID
        std::string fromPort;    // Source output port name
        size_t toNode;           // Destination node UID
        std::string toPort;      // Destination input port name

        NodeConnection() = default;
        NodeConnection(size_t from, std::string fromP, size_t to, std::string toP)
            : fromNode(from), fromPort(std::move(fromP))
            , toNode(to), toPort(std::move(toP)) {}
    };

    /**
     * @brief Result of evaluating an entire node graph
     */
    struct GraphEvaluationResult
    {
        // Named outputs from the graph (e.g., "geometry" -> result)
        std::unordered_map<std::string, NodeEvaluationResult> outputs;

        // All node evaluation results (keyed by node UID)
        // This allows accessing any node's result, not just named outputs
        std::unordered_map<size_t, NodeEvaluationResult> nodeResults;

        bool success = true;
        std::string error;

        GraphEvaluationResult() = default;

        static GraphEvaluationResult makeError(const std::string& errorMsg) {
            GraphEvaluationResult result;
            result.success = false;
            result.error = errorMsg;
            return result;
        }
    };

    /**
     * @brief Container and manager for procedural nodes
     *
     * Manages:
     * - Node storage (flat map, UID-based lookup)
     * - Connections between nodes
     * - Evaluation order (topological sort)
     * - Named output nodes
     *
     * Phase 1: Basic node storage and retrieval
     * Phase 2: Connection management and evaluation
     */
    class NodeGraph : public Graph
    {
    public:
        NodeGraph(size_t uid, std::string name);
        virtual ~NodeGraph() = default;

        const std::string& name() const { return m_name; }
        void setName(const std::string& name) { m_name = name; }

        // Node CRUD operations
        ProceduralNode* createNode(NodeType type, const std::string& name);
        ProceduralNode* getNode(size_t uid);
        const ProceduralNode* getNode(size_t uid) const;
        bool removeNode(size_t uid);

        // Get all nodes
        const std::unordered_map<size_t, std::unique_ptr<ProceduralNode>>& nodes() const {
            return m_nodes;
        }

        // Connection management (Phase 2)
        // Currently stubbed for Phase 1
        bool connect(size_t fromNode, const std::string& fromPort,
                    size_t toNode, const std::string& toPort);
        bool disconnect(size_t fromNode, const std::string& fromPort,
                       size_t toNode, const std::string& toPort);
        const std::vector<NodeConnection>& connections() const {
            return m_connections;
        }

        // Output nodes (special nodes that define graph outputs)
        void setOutputNode(const std::string& outputName, size_t nodeUid);
        ProceduralNode* getOutputNode(const std::string& outputName);
        const std::unordered_map<std::string, size_t>& outputNodes() const {
            return m_outputNodes;
        }

        // Graph evaluation (Phase 2)
        // For Phase 1, this is stubbed - just returns empty result
        GraphEvaluationResult evaluate(const EvaluationContext& ctx);

        // Dirty propagation
        void markDirty();
        void markDirtyDownstream(size_t nodeUid);

        // UID generation for new nodes
        size_t generateNodeUid();

        // Phase 2: Parent graph tracking (for nested graphs)
        NodeGraph* parent() const { return m_parent; }
        void setParent(NodeGraph* parent) { m_parent = parent; }

        // Phase 2: Owner node tracking (ActorNode that owns this geometry network)
        size_t ownerNodeUid() const { return m_ownerNodeUid; }
        void setOwnerNodeUid(size_t uid) { m_ownerNodeUid = uid; }

        // Phase 2: Graph context queries
        bool isSceneLevelGraph() const { return m_parent == nullptr && m_ownerNodeUid == 0; }
        bool isGeometryNetwork() const { return m_parent != nullptr || m_ownerNodeUid != 0; }

    private:
        std::string m_name;
        std::unordered_map<size_t, std::unique_ptr<ProceduralNode>> m_nodes;
        std::vector<NodeConnection> m_connections;
        std::unordered_map<std::string, size_t> m_outputNodes;  // output name -> node UID
        size_t m_nextNodeUid = 1;

        // Phase 2: Parent tracking for nested graphs
        NodeGraph* m_parent = nullptr;      // Parent graph (nullptr for scene graph)
        size_t m_ownerNodeUid = 0;          // UID of ActorNode that owns this graph (0 for scene)

        // Phase 2: Topological sort for evaluation order
        std::vector<size_t> computeEvaluationOrder() const;
    };

} // namespace tracey
