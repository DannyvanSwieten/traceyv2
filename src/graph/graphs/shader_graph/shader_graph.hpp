#pragma once
#include "../../graph.hpp"
#include "shader_graph_node.hpp"

#include <optional>
#include <utility>

namespace tracey
{
    // A ShaderGraph is the user-authored material description: typed nodes
    // (Constant, Parameter, BinaryOp, ..., Output) connected by Connection
    // edges. Compiling a ShaderGraph produces a MaterialProgram that runs in
    // the VM at shading time.
    //
    // Storage is inherited from Graph (m_nodes, m_connections); ShaderGraph
    // adds typed accessors and traversal helpers used by the compiler.
    class ShaderGraph : public Graph
    {
    public:
        explicit ShaderGraph(size_t uid = 0);

        // Look up a ShaderGraphNode by its uid. Returns nullptr if not found
        // or if the node isn't a ShaderGraphNode (shouldn't happen in
        // well-formed graphs).
        const ShaderGraphNode *findNode(size_t uid) const;

        // Find the connection feeding into `nodeUid`'s `inputPortIdx`. Returns
        // {fromNodeUid, fromPortIdx} or nullopt when the input is not connected.
        // (Linear scan over connections — acceptable for the small graphs we
        // expect; if graphs grow large, build an index.)
        std::optional<std::pair<size_t, size_t>>
        incomingTo(size_t nodeUid, size_t inputPortIdx) const;
    };
}
