#pragma once

#include "../graph/graph.hpp"
#include "dop_node.hpp"
#include "sim_state.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace tracey
{
    namespace dops
    {
        // A simulation graph — the top-level peer of the root SopGraph.
        //
        // Cook model:
        //   1. compile() — Kahn's topo sort over nodes (mirrors VopGraph).
        //      Idempotent until markDirty().
        //   2. cookFrame(prev, frameIdx, fps, substeps) — produces the
        //      SimState for `frameIdx` given the prior frame's state. Walks
        //      each node's cookFrame() in topo order, once per substep.
        //   3. cookToFrame(target, fps, substeps) — fills the per-frame
        //      cache from the last-cached frame forward to `target`. Cheap
        //      when the cache already covers the target (just a lookup).
        //
        // State cache: keyed by integer frame number. Frame 0 is the implicit
        // empty "before sim starts" state. Frame 1 is the first cooked frame.
        // Edits anywhere in the graph (params, nodes, connections) MUST call
        // markDirty() — which also clears the cache, since prior frames were
        // derived from the old graph and are now invalid.
        //
        // Phase 0: no inter-node data ports. cookFrame walks nodes linearly
        // in topo order; each node mutates the shared SimState. Adding data
        // slots later is purely additive — mirror VopGraph's slot table and
        // readInput/writeOutput then.
        class DopGraph : public Graph
        {
        public:
            explicit DopGraph(size_t uid = 0);

            DopNode *findNode(size_t uid);
            const DopNode *findNode(size_t uid) const;

            size_t nextUid();
            size_t maxNodeUid() const;
            void setNextUid(size_t uid) { m_nextUid = uid; }

            // Topo-sorted node uids. Empty until compile() runs (or if the
            // graph has a cycle).
            const std::vector<size_t> &topoOrder() const { return m_topoOrder; }

            // Mark the topo cache stale AND invalidate the frame cache.
            // Cheap; the next cookToFrame() rebuilds. Call after any
            // structural or parameter edit.
            void markDirty();

            // Build the topo cache. Const because cook is const — mutates
            // only the mutable members.
            void compile() const;

            // Highest frame index currently in the cache, or 0 if empty.
            // Used by the editor to draw a "cached up to frame N" bar.
            int cachedToFrame() const;

            // Return the cached SimState for a frame, or nullptr if not
            // cached. Reads are cheap (hashmap lookup); the caller should
            // NOT mutate the returned state — it's the canonical cache
            // entry that downstream consumers (`dop_import` SOP) will read.
            const SimState *frame(int frameIdx) const;

            // Cook frame-by-frame from the last-cached frame forward to
            // `target`. No-op when `target <= cachedToFrame()`. fps and
            // substepsPerFrame come from the timeline / solver params.
            void cookToFrame(int target, double fps, int substepsPerFrame = 1);

            // Drop every cached frame. The next cookToFrame() will restart
            // from the empty frame-0 baseline. Used by the "Reset Sim"
            // button and by markDirty().
            void clearCache();

        private:
            // One frame's cook, given the prior frame's state. Returns the
            // new SimState. Walks node topo order over `substeps` substeps.
            SimState cookOneFrame(const SimState &prev,
                                  int frameIdx,
                                  double fps,
                                  int substepsPerFrame) const;

            size_t m_nextUid = 1;

            mutable bool m_dirty = true;
            mutable std::vector<size_t> m_topoOrder;

            // Per-frame cache. Dense vector indexed by frame number; index 0
            // is the implicit empty initial state. Resized on the fly as we
            // cook forward.
            std::vector<SimState> m_frameCache;
        };
    }
}
