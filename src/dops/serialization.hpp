#pragma once

#include "dop_graph.hpp"

#include <memory>
#include <string>

namespace tracey
{
    namespace dops
    {
        // JSON serialization for DopGraph. Schema is identical to
        // SopGraph / VopGraph (see src/vops/serialization.hpp) modulo
        // `graph_kind: "dop"` and the DOP-specific node kinds. Public API
        // takes/returns std::string so consumers don't pull in nlohmann/json.

        std::string serializeDopGraph(const DopGraph &graph);
        std::string serializeDopGraphPretty(const DopGraph &graph);

        // Throws std::runtime_error on bad JSON / unknown node kinds.
        std::unique_ptr<DopGraph> deserializeDopGraph(const std::string &jsonText);
    }
}
