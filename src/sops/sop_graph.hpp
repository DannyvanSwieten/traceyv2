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
            size_t nextUid();

            // Cook the graph.
            //
            //   1. Topological sort over nodes; cycle → diag.ok=false.
            //   2. For each node in order, gather pointers to already-cooked
            //      input geometries (or nullptr if the input port is
            //      unconnected) and call cook().
            //   3. Sweep ObjectOutput nodes; collect their EmittedActors.
            //
            // Result cache lives until invalidate() is called (on graph edit).
            CookResult cook(CookDiagnostic *diag = nullptr);

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
            // Per-node cooked geometry, keyed on uid. Cleared by invalidate().
            std::unordered_map<size_t, Geometry> m_cache;
        };
    }
}
