#include "node_graph.hpp"
#include <algorithm>
#include <unordered_set>

namespace tracey
{
    NodeGraph::NodeGraph(size_t uid, std::string name)
        : Graph(uid)
        , m_name(std::move(name))
        , m_nextNodeUid(1)
    {
    }

    ProceduralNode* NodeGraph::createNode(NodeType type, const std::string& name)
    {
        // Phase 1: Basic node creation
        // For now, we can't actually instantiate ProceduralNode since it's abstract
        // Derived classes (ActorNode, PrimitiveNode, etc.) will be created instead
        // This method will be implemented properly when we have concrete node types

        // TODO: Factory pattern to create specific node types
        (void)type;
        (void)name;
        return nullptr;
    }

    ProceduralNode* NodeGraph::getNode(size_t uid)
    {
        auto it = m_nodes.find(uid);
        return (it != m_nodes.end()) ? it->second.get() : nullptr;
    }

    const ProceduralNode* NodeGraph::getNode(size_t uid) const
    {
        auto it = m_nodes.find(uid);
        return (it != m_nodes.end()) ? it->second.get() : nullptr;
    }

    bool NodeGraph::removeNode(size_t uid)
    {
        // Remove the node
        auto nodeIt = m_nodes.find(uid);
        if (nodeIt == m_nodes.end()) {
            return false;
        }

        // Remove all connections involving this node
        m_connections.erase(
            std::remove_if(m_connections.begin(), m_connections.end(),
                [uid](const NodeConnection& conn) {
                    return conn.fromNode == uid || conn.toNode == uid;
                }),
            m_connections.end()
        );

        // Remove from output nodes if it's an output
        for (auto it = m_outputNodes.begin(); it != m_outputNodes.end();) {
            if (it->second == uid) {
                it = m_outputNodes.erase(it);
            } else {
                ++it;
            }
        }

        m_nodes.erase(nodeIt);
        return true;
    }

    bool NodeGraph::connect(size_t fromNode, const std::string& fromPort,
                           size_t toNode, const std::string& toPort)
    {
        // Phase 2: Connection management
        // For Phase 1, just store the connection

        // Verify nodes exist
        if (!getNode(fromNode) || !getNode(toNode)) {
            return false;
        }

        // Check for duplicate connection
        for (const auto& conn : m_connections) {
            if (conn.fromNode == fromNode && conn.fromPort == fromPort &&
                conn.toNode == toNode && conn.toPort == toPort) {
                return false;  // Already connected
            }
        }

        m_connections.emplace_back(fromNode, fromPort, toNode, toPort);

        // Mark destination node as dirty
        if (auto* node = getNode(toNode)) {
            node->setDirty(true);
        }

        return true;
    }

    bool NodeGraph::disconnect(size_t fromNode, const std::string& fromPort,
                              size_t toNode, const std::string& toPort)
    {
        // Phase 2: Connection management
        auto it = std::find_if(m_connections.begin(), m_connections.end(),
            [&](const NodeConnection& conn) {
                return conn.fromNode == fromNode && conn.fromPort == fromPort &&
                       conn.toNode == toNode && conn.toPort == toPort;
            });

        if (it != m_connections.end()) {
            m_connections.erase(it);

            // Mark destination node as dirty
            if (auto* node = getNode(toNode)) {
                node->setDirty(true);
            }

            return true;
        }

        return false;
    }

    void NodeGraph::setOutputNode(const std::string& outputName, size_t nodeUid)
    {
        m_outputNodes[outputName] = nodeUid;
    }

    ProceduralNode* NodeGraph::getOutputNode(const std::string& outputName)
    {
        auto it = m_outputNodes.find(outputName);
        if (it != m_outputNodes.end()) {
            return getNode(it->second);
        }
        return nullptr;
    }

    GraphEvaluationResult NodeGraph::evaluate(const EvaluationContext& ctx)
    {
        GraphEvaluationResult result;

        // Get evaluation order via topological sort
        std::vector<size_t> evaluationOrder = computeEvaluationOrder();

        // Check for cycles
        if (evaluationOrder.empty() && !m_nodes.empty()) {
            return GraphEvaluationResult::makeError("Cycle detected in node graph");
        }

        // Create a mutable context for caching results
        EvaluationContext mutableCtx = ctx;
        mutableCtx.graph = this;

        // Evaluate nodes in topological order
        for (size_t nodeUid : evaluationOrder) {
            auto* node = getNode(nodeUid);
            if (!node) {
                continue;
            }

            // Skip evaluation if node is clean (not dirty)
            if (!node->isDirty()) {
                continue;
            }

            // Evaluate the node
            NodeEvaluationResult nodeResult = node->evaluate(mutableCtx);

            // Cache the result
            mutableCtx.cache[nodeUid] = nodeResult;

            // Mark node as clean
            node->setDirty(false);

            // Check for errors
            if (!nodeResult.success) {
                return GraphEvaluationResult::makeError(
                    "Node '" + node->name() + "' failed: " + nodeResult.error
                );
            }
        }

        // Gather output node results
        for (const auto& [outputName, nodeUid] : m_outputNodes) {
            auto cacheIt = mutableCtx.cache.find(nodeUid);
            if (cacheIt != mutableCtx.cache.end()) {
                result.outputs[outputName] = cacheIt->second;
            }
        }

        return result;
    }

    void NodeGraph::markDirty()
    {
        for (auto& [uid, node] : m_nodes) {
            node->setDirty(true);
        }
    }

    void NodeGraph::markDirtyDownstream(size_t nodeUid)
    {
        // Mark the node dirty
        if (auto* node = getNode(nodeUid)) {
            node->setDirty(true);
        }

        // Find all nodes connected to this node's outputs and mark them dirty
        std::vector<size_t> toProcess = { nodeUid };
        std::unordered_set<size_t> processed;

        while (!toProcess.empty()) {
            size_t currentUid = toProcess.back();
            toProcess.pop_back();

            if (processed.find(currentUid) != processed.end()) {
                continue;
            }
            processed.insert(currentUid);

            // Find all connections from this node
            for (const auto& conn : m_connections) {
                if (conn.fromNode == currentUid) {
                    if (auto* downstreamNode = getNode(conn.toNode)) {
                        downstreamNode->setDirty(true);
                        toProcess.push_back(conn.toNode);
                    }
                }
            }
        }
    }

    size_t NodeGraph::generateNodeUid()
    {
        return m_nextNodeUid++;
    }

    std::vector<size_t> NodeGraph::computeEvaluationOrder() const
    {
        // Topological sort using Kahn's algorithm
        std::vector<size_t> result;
        std::unordered_map<size_t, int> inDegree;
        std::unordered_map<size_t, std::vector<size_t>> adjacencyList;

        // Initialize in-degree for all nodes
        for (const auto& [uid, node] : m_nodes) {
            inDegree[uid] = 0;
            adjacencyList[uid] = {};
        }

        // Build adjacency list and count in-degrees
        for (const auto& conn : m_connections) {
            adjacencyList[conn.fromNode].push_back(conn.toNode);
            inDegree[conn.toNode]++;
        }

        // Find all nodes with in-degree 0
        std::vector<size_t> queue;
        for (const auto& [uid, degree] : inDegree) {
            if (degree == 0) {
                queue.push_back(uid);
            }
        }

        // Process nodes in topological order
        while (!queue.empty()) {
            size_t current = queue.back();
            queue.pop_back();
            result.push_back(current);

            // Reduce in-degree for connected nodes
            for (size_t neighbor : adjacencyList[current]) {
                inDegree[neighbor]--;
                if (inDegree[neighbor] == 0) {
                    queue.push_back(neighbor);
                }
            }
        }

        // If result doesn't contain all nodes, there's a cycle
        if (result.size() != m_nodes.size()) {
            // Return empty to indicate cycle detection
            return {};
        }

        return result;
    }

} // namespace tracey
