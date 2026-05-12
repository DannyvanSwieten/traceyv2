#pragma once

#include "../core/types.hpp"

#include <cstddef>
#include <variant>
#include <vector>

namespace tracey
{
    class Geometry;

    namespace vops
    {
        class VopGraph;

        // Carried through every per-point evaluation. `slots` is a flat array
        // sized to VopGraph::slotCount() — one entry per (nodeUid, outputPort)
        // pair, indexed via VopGraph::slotIndex(). Zero-initialised at the
        // start of each point so cross-point leakage is impossible.
        //
        // Value is intentionally narrow for v1: float / Vec3 / int. Add Vec4
        // or other types only when a node needs them, and keep the same
        // index-by-discriminant approach to avoid unbounded variant growth.
        using Value = std::variant<float, Vec3, int>;

        struct EvalContext
        {
            size_t pointIndex = 0;
            Geometry *geometry = nullptr;
            std::vector<Value> *slots = nullptr;
            const VopGraph *graph = nullptr;
        };
    }
}
