#pragma once

#include "sop_graph.hpp"

#include <memory>
#include <string>

namespace tracey
{
    namespace sops
    {
        // JSON serialization for SopGraph. Public API takes/returns std::string
        // so consumers don't pull in nlohmann::json transitively.
        //
        // Schema (version 1):
        //   {
        //     "graph_kind": "sop",
        //     "version": 1,
        //     "uid": <graph uid>,
        //     "next_uid": <next-allocatable node uid>,
        //     "nodes": [
        //       {
        //         "uid": 1,
        //         "kind": "primitive_cube",
        //         "pos": [120.0, 80.0],
        //         "params": {
        //            "size": {"type": "float", "value": 1.0},
        //            // Animated parameter: optional `channels` array. Length is
        //            // 1 for scalar params and 3 for vec3 (one per component);
        //            // null entries mean that component is not animated.
        //            "translate": {
        //              "type": "vec3",
        //              "value": [0, 0, 0],
        //              "channels": [
        //                {
        //                  "keys": [{"t": 0.0, "v": 0.0, "in": 0.0, "out": 0.0, "i": "linear"}],
        //                  "pre":  "hold",
        //                  "post": "hold"
        //                },
        //                null,
        //                null
        //              ]
        //            }
        //         }
        //       },
        //       ...
        //     ],
        //     "connections": [
        //       {"from_node": 1, "from_port": 0, "to_node": 2, "to_port": 0}
        //     ]
        //   }
        //
        // Param values are tagged with their type so a future codegen step can
        // emit C++ literals from the same JSON without round-tripping through
        // the variant. Types: "float" | "int" | "bool" | "vec3" | "string".

        std::string serializeSopGraph(const SopGraph &graph);
        std::string serializeSopGraphPretty(const SopGraph &graph);

        // Throws std::runtime_error on bad JSON / unknown node kinds / missing
        // required fields.
        std::unique_ptr<SopGraph> deserializeSopGraph(const std::string &jsonText);
    }
}
