#include "output_node.hpp"

namespace tracey
{
    OutputNode::OutputNode(size_t uid)
        : Node(uid)
    {
    }
    OutputPrototype::OutputPrototype(std::string name, size_t version, DataType dataType) : Prototype(std::move(name), version), m_dataType(dataType)
    {
    }
    std::unique_ptr<Node> OutputPrototype::create() const
    {
        return std::make_unique<OutputNode>(0); // UID should be set by the graph managing this node
    }
    Template OutputPrototype::defaultTemplate() const
    {
        Template t;
        t.addArgument({m_dataType});
        return t;
    }
}