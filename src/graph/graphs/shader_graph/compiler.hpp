#pragma once
#include "shader_graph.hpp"
#include "../../../shading/material_program/material_program.hpp"

namespace tracey
{
    // Compile a ShaderGraph into a MaterialProgram.
    //
    // Strategy:
    //   1. Topologically sort the graph's nodes (producer before consumer).
    //   2. Walk nodes in order, assigning one register per producing output
    //      port. Constants/parameters/attributes emit a load and write into
    //      a fresh register. Math ops read source registers via the graph's
    //      incomingTo(...) lookup. Output nodes emit a Write* terminator.
    //   3. parameterDefaults are collected from ParameterNode default values
    //      and carried on the MaterialProgram so the host can seed the
    //      parameters pool when the program is added to a buffer.
    //
    // Throws std::runtime_error on:
    //   - cycles in the graph
    //   - missing connections into a non-Output node's input port
    //   - a node referencing an opcode outside its allowed family
    //     (e.g. a BinaryOpNode carrying Op::Mix)
    MaterialProgram compileShaderGraph(const ShaderGraph &graph);
}
