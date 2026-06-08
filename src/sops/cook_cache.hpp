#pragma once

#include "../geometry/geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace tracey
{
    namespace sops
    {
        // Persistent, per-node geometry cache for SopGraph::cook.
        //
        // Houdini-style cook lifecycle:
        //   • Each node's cooked Geometry is stored under its uid.
        //   • Each entry tracks an `inputKey` — a 64-bit fingerprint of
        //     (kind, params, incoming connections + upstream cookIds, time
        //     if the subtree depends on time) — and a monotonically-bumping
        //     `cookId` that increments only on actual re-cooks.
        //   • On the next cook, SopGraph::cook computes each node's new
        //     inputKey. If it matches the cached entry's stored key, the
        //     cached Geometry is reused and `cook()` isn't invoked at all.
        //   • Downstream nodes mix this entry's `cookId` into their own
        //     inputKey, so an upstream re-cook propagates naturally.
        //
        // Entries persist across `set_sop_graph` deserializations because the
        // cache lives in the editor server, not on the SopGraph itself; uids
        // are globally unique so the keying survives a graph swap intact.
        //
        // Thread safety: not synchronized. The editor server gives the cook
        // worker thread its own instance and serializes access through the
        // existing cook-request CV — concurrent cooks can't race.
        class CookCache
        {
        public:
            struct Entry
            {
                // Bumps every time the node was actually re-cooked. Stays
                // unchanged across cache hits, so downstream nodes mixing
                // this in see a stable signature for clean upstreams.
                uint64_t cookId = 0;
                // Hash of (kind, params, incoming connection topology +
                // upstream cookIds, time if timeDependent). Compared to a
                // freshly computed key to decide hit vs miss.
                uint64_t inputKey = 0;
                // Last cooked output. Read by downstream cooks when this
                // entry is a cache hit; downstream pointers must stay valid
                // for the duration of the cook (see SopGraph::cook).
                Geometry output;
                // True if this node or any upstream node reads time (e.g.
                // an attribute_vop with a @Time-reading VOP). Causes the
                // inputKey computation to include the cook time.
                bool timeDependent = false;
                // False until the first cook stores into this entry. A
                // freshly-allocated entry returned from `upsert()` starts
                // false so an incidental find() can't trigger a false hit.
                bool valid = false;
                // Set during the current cook when the entry was visited
                // (either as a hit or refreshed by a miss). evictUntouched()
                // drops anything still false.
                bool touched = false;
            };

            // Returns the existing entry pointer for `uid`, or nullptr.
            // Side effect: marks the entry touched on hit.
            Entry *find(size_t uid);

            // Read-only lookup for external consumers (e.g. the DOP
            // graph's SopGeometryProvider during a sim cook). Returns
            // the last cooked Geometry for `uid`, or nullptr if absent /
            // not yet cooked. Does NOT mark the entry touched — the
            // touched flag is owned by the SOP cook's own visitation
            // sweep and shouldn't be flipped by side-channel reads.
            const Geometry *findOutput(size_t uid) const;

            // Get-or-create. The returned entry is marked touched; if it's
            // freshly constructed, `valid` is still false until the caller
            // fills in inputKey + output + flips valid = true.
            Entry &upsert(size_t uid);

            void markAllUntouched();
            void evictUntouched();
            void clear() { m_entries.clear(); }
            size_t size() const { return m_entries.size(); }

        private:
            std::unordered_map<size_t, Entry> m_entries;
        };
    }
}
