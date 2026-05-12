#pragma once

#include "../graph/graph.hpp"
#include "../geometry/geometry.hpp"
#include "sop_node.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tracey
{
    namespace sops
    {
        // Result of cooking the graph — collected from terminal ObjectOutput
        // nodes during the topo walk and returned to the caller for actor
        // construction.
        using CookResult = std::vector<EmittedActor>;

        struct CookDiagnostic
        {
            bool ok = true;
            std::string message;
            // uid of the offending node, if applicable. 0 means "graph-level".
            size_t nodeUid = 0;
        };

        class SopGraph : public Graph
        {
        public:
            explicit SopGraph(size_t uid = 0);

            // Look up a SopNode by uid. Returns nullptr if not found or if
            // the node isn't a SopNode (shouldn't happen in well-formed graphs).
            SopNode *findNode(size_t uid);
            const SopNode *findNode(size_t uid) const;

            // Connection feeding `nodeUid`'s `inputPortIdx`. nullopt when
            // unconnected.
            std::optional<std::pair<size_t, size_t>>
            incomingTo(size_t nodeUid, size_t inputPortIdx) const;

            // Allocate the next node uid. Caller is expected to construct the
            // node with this uid then `addNode(...)` it.
            //
            // Inner subnet sub-graphs forward to a root allocator (see
            // setRoot) so node uids stay globally unique across nesting —
            // the editor IPC and animation channel system identify nodes by
            // uid alone with no path qualifier.
            size_t nextUid();

            // Subnet sub-graphs call this with the outer (root) graph so
            // their nextUid() forwards. Pass nullptr (default) to detach.
            void setRoot(SopGraph *root) { m_root = root; }
            SopGraph *root() { return m_root ? m_root : this; }

            // Cook the graph.
            //
            //   1. Topological sort over nodes; cycle → diag.ok=false.
            //   2. For each node in order, gather pointers to already-cooked
            //      input geometries (or nullptr if the input port is
            //      unconnected) and call cookAt(inputs, time).
            //   3. Sweep ObjectOutput nodes; collect their EmittedActors.
            //   4. Sweep subnet nodes; recursively cook inner graphs with the
            //      same time, stamping parentNodeUid on each emitted child.
            //
            // `time` is the playhead time in seconds. Threading it through the
            // cook lets time-aware nodes (today: attribute_vop with promoted
            // host params) sample their parameters at the current frame. Most
            // SopNode subclasses ignore it and the default `cookAt` delegates
            // to the time-independent `cook`.
            //
            // Result cache lives until invalidate() is called (on graph edit).
            CookResult cook(CookDiagnostic *diag = nullptr, double time = 0.0);

            // Drop all cached cook results. Call after any structural or
            // parameter change. (Editor host does this implicitly on every
            // set_sop_graph push.)
            void invalidate();

            // ── Helpers used by serialization / the editor host ──
            // Returns the highest current node uid in the graph (or 0 if empty).
            // Used to seed nextUid() after deserializing.
            size_t maxNodeUid() const;
            void setNextUid(size_t uid) { m_nextUid = uid; }

        private:
            size_t m_nextUid = 1;
            // Non-null on inner sub-graphs (subnets); the root allocator
            // owns the canonical uid sequence. nullptr on the outermost
            // SopGraph.
            SopGraph *m_root = nullptr;
            // Per-node cooked geometry, keyed on uid. Cleared by invalidate().
            std::unordered_map<size_t, Geometry> m_cache;
        };
    }
}
