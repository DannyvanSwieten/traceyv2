#pragma once

#include "vop_graph.hpp"

#include <memory>
#include <string>

namespace tracey
{
    namespace vops
    {
        // JSON serialization for VopGraph. Schema is identical to SopGraph's
        // (see src/sops/serialization.hpp) modulo `graph_kind: "vop"` and the
        // VOP-specific node kinds. Public API takes/returns std::string so
        // consumers don't pull in nlohmann::json transitively.

        std::string serializeVopGraph(const VopGraph &graph);
        std::string serializeVopGraphPretty(const VopGraph &graph);

        // Throws std::runtime_error on bad JSON / unknown node kinds.
        std::unique_ptr<VopGraph> deserializeVopGraph(const std::string &jsonText);
    }
}
