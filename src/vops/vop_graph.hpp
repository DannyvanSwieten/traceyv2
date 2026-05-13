#pragma once

#include "../graph/graph.hpp"
#include "eval_context.hpp"
#include "typing.hpp"
#include "vop_node.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tracey
{
    namespace vops
    {
        // A per-point attribute graph hosted inside an `attribute_vop` SOP.
        //
        // Cook model:
        //   1. compile() — runs Kahn's topo sort, then assigns each
        //      (nodeUid, outputPort) pair a dense index into a per-cook value
        //      array. Idempotent (early-returns when m_dirty is false).
        //   2. evaluatePoint(idx, geo) — for the given point: zero the slot
        //      array, walk nodes in topo order, dispatch each node's
        //      evaluate(EvalContext&). Reads / writes attribute values from
        //      the geometry directly via the geo_input / geo_output
        //      terminal node kinds.
        //
        // Mutating the graph (addNode / addConnection / setParamFloat / ...)
        // must call markDirty() so the next compile() rebuilds the slot table.
        class VopGraph : public Graph
        {
        public:
            explicit VopGraph(size_t uid = 0);

            VopNode *findNode(size_t uid);
            const VopNode *findNode(size_t uid) const;

            std::optional<std::pair<size_t, size_t>>
            incomingTo(size_t nodeUid, size_t inputPortIdx) const;

            size_t nextUid();
            size_t maxNodeUid() const;
            void setNextUid(size_t uid) { m_nextUid = uid; }

            // Slot table built by compile(). slotIndex(nodeUid, outputPort)
            // returns the dense index into EvalContext::slots, or SIZE_MAX
            // if the (uid, port) pair isn't known to this graph.
            size_t slotCount() const { return m_slotCount; }
            size_t slotIndex(size_t nodeUid, size_t outputPort) const;

            // Topo-sorted node uids. Empty until compile() runs.
            const std::vector<size_t> &topoOrder() const { return m_topoOrder; }

            // Mark the cached topo + slot table stale. Cheap; the next
            // compile() rebuilds. Call after any structural or parameter edit.
            void markDirty() { m_dirty = true; }

            // Build (or rebuild on m_dirty) the topo order and slot table.
            // No-op when already current. Const because cooks are const —
            // mutates only the mutable cache members.
            void compile() const;

            // Inferred per-port types — see typing.hpp. Populated as a side
            // effect of compile() so both the CPU evaluator and the GPU
            // emitter consume the same answer for any polymorphic node
            // (add, fit, clamp, mix, ...). Before this existed each path
            // ran its own ad-hoc promotion heuristic and the two could
            // disagree on a graph that wired a vec3 into a "float" port.
            TypeKind portType(size_t nodeUid, size_t port, bool isOutput) const;

            // Per-point evaluation. Caller is expected to have called
            // compile() at least once; we call it lazily here too.
            void evaluatePoint(size_t pointIdx, Geometry &geo) const;

            // Same, but the caller supplies the per-point slot buffer so
            // it can be reused across points (and per-thread for the
            // parallel-cook path). The buffer is unconditionally resized
            // + zeroed to slotCount() each call.
            void evaluatePoint(size_t pointIdx, Geometry &geo,
                               std::vector<Value> &slots) const;

            // Resolve the upstream (nodeUid, outputPort) feeding this node's
            // input port, then read its slot. Returns nullopt if unconnected.
            std::optional<Value> readInput(const EvalContext &ctx,
                                           size_t nodeUid,
                                           size_t inputPortIdx) const;

            // Write a node's own output value into the per-cook slot array.
            void writeOutput(EvalContext &ctx, size_t nodeUid,
                             size_t outputPortIdx, Value v) const;

        private:
            size_t m_nextUid = 1;

            // Mutable cache populated by compile(). Mutating the graph must
            // markDirty() so we rebuild on next cook.
            mutable bool m_dirty = true;
            mutable std::vector<size_t> m_topoOrder;
            mutable size_t m_slotCount = 0;
            mutable std::unordered_map<uint64_t, size_t> m_slotIndex; // (uid<<32)|port → slot
            mutable TypeInferenceResult m_types;

            static uint64_t makeSlotKey(size_t nodeUid, size_t port)
            {
                return (static_cast<uint64_t>(nodeUid) << 32) |
                       (static_cast<uint64_t>(port) & 0xFFFFFFFFu);
            }
        };
    }
}
