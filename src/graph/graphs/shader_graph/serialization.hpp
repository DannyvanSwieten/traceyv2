#pragma once
#include "shader_graph.hpp"

#include <memory>
#include <string>

namespace tracey
{
    // JSON serialization for ShaderGraph. Public API takes/returns std::string
    // so consumers don't pull in nlohmann::json transitively.
    //
    // Schema (version 1):
    //   {
    //     "version": 1,
    //     "uid": <graph uid, integer>,
    //     "nodes": [
    //       {"uid": 1, "kind": "Constant",         "value": [r, g, b, a]},
    //       {"uid": 2, "kind": "Parameter",        "name": "...", "default": [r, g, b, a]},
    //       {"uid": 3, "kind": "SurfaceAttribute", "op": "LoadNormal"},
    //       {"uid": 4, "kind": "InputAttribute",   "op": "LoadInputAlbedo"},
    //       {"uid": 5, "kind": "BinaryOp",         "op": "Add"},
    //       {"uid": 6, "kind": "UnaryOp",          "op": "Saturate"},
    //       {"uid": 7, "kind": "TernaryOp",        "op": "Mix"},
    //       {"uid": 8, "kind": "Output",           "op": "WriteAlbedo"}
    //     ],
    //     "connections": [
    //       {"from_node": 1, "from_port": 0, "to_node": 8, "to_port": 0}
    //     ]
    //   }
    //
    // Op and kind values are spelled exactly as the C++ enumerator name (without
    // the enum class prefix), e.g. "Add", "LoadInputAlbedo", "WriteAlbedo".

    // Serialize to a compact (single-line) JSON string.
    std::string serializeShaderGraph(const ShaderGraph &graph);

    // Serialize to a pretty-printed JSON string. Useful for files the user
    // will hand-edit.
    std::string serializeShaderGraphPretty(const ShaderGraph &graph);

    // Deserialize a JSON string. Throws std::runtime_error on parse failures,
    // unknown kind/op tokens, or missing required fields.
    std::unique_ptr<ShaderGraph> deserializeShaderGraph(const std::string &jsonText);
}
