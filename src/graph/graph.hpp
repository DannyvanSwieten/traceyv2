#pragma once
#include <vector>
#include <memory>
#include "connection.hpp"
namespace tracey
{
    class Node;
    // This is a graph structure base class. Specific graph implementations
    // will derive from this class. The top level graph will probably be a scene graph.
    // Below that could other types of graphs like shader graphs, animation graphs, etc.
    class Graph
    {
    public:
        Graph(size_t uid) : m_uid(uid) {}
        virtual ~Graph();

        void createConnection(size_t fromNode, size_t fromPort, size_t toNode, size_t toPort);
        void addConnection(const Connection &connection);

        void addNode(std::unique_ptr<Node> node);

        size_t uid() const;

    private:
        size_t m_uid;
        std::vector<std::unique_ptr<Node>> m_nodes;
        std::vector<Connection> m_connections;
    };
}