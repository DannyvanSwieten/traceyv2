#include "shader_graph.hpp"

namespace tracey
{
    ShaderGraph::ShaderGraph(size_t uid) : Graph(uid) {}

    const ShaderGraphNode *ShaderGraph::findNode(size_t uid) const
    {
        for (const auto &node : nodes())
        {
            if (node->uid() == uid)
            {
                return dynamic_cast<const ShaderGraphNode *>(node.get());
            }
        }
        return nullptr;
    }

    std::optional<std::pair<size_t, size_t>>
    ShaderGraph::incomingTo(size_t nodeUid, size_t inputPortIdx) const
    {
        for (const auto &c : connections())
        {
            if (c.toNode == nodeUid && c.toPort == inputPortIdx)
            {
                return std::make_pair(c.fromNode, c.fromPort);
            }
        }
        return std::nullopt;
    }
}
