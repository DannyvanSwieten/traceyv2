#include "graph.hpp"
#include "node.hpp"
namespace tracey
{
    Graph::~Graph()
    {
    }

    void Graph::createConnection(size_t fromNode, size_t fromPort, size_t toNode, size_t toPort)
    {
        m_connections.push_back({fromNode, fromPort, toNode, toPort});
    }
    void Graph::addConnection(const Connection &connection)
    {
        m_connections.push_back(connection);
    }
    void Graph::addNode(std::unique_ptr<Node> node)
    {
        m_nodes.emplace_back(std::move(node));
    }
    size_t Graph::uid() const
    {
        return m_uid;
    }
}