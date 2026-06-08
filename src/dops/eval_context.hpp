#pragma once

#include <cstdint>

#include "sim_state.hpp"

namespace tracey
{
    class Geometry;

    namespace dops
    {
        class DopGraph;

        // Provides DOPs read access to the SOP graph's last cook output. The
        // editor implements this over its live SOP cook cache; smoke tests
        // implement it ad-hoc over a stored map. Lookup is by SOP node uid
        // so the binding survives the source node being renamed (the editor
        // picker stores the uid, not the name).
        //
        // Returns nullptr when the uid isn't in the cache (the source SOP
        // hasn't been cooked yet, or was deleted). Consumers must handle
        // null — e.g. pop_source falls back to its origin/initial_v defaults.
        class SopGeometryProvider
        {
        public:
            virtual ~SopGeometryProvider() = default;
            virtual const Geometry *lookupCookedGeometry(uint64_t sopUid) const = 0;
        };

        // Carried through every DOP node's cookFrame() call.
        //
        //   state — the SimState being built up for the current (frame,
        //           substep). Nodes mutate it in place: pop_source appends
        //           points, pop_gravity accumulates into `force`, pop_solver
        //           integrates v and P. Always non-null inside a cook.
        //   graph — the DopGraph driving the cook. Used by nodes that need
        //           to introspect (rare in Phase 0 — kept for parity with
        //           VopGraph's EvalContext shape).
        //   sopProvider — optional. When set, geometry-source DOPs (e.g.
        //           pop_source's emit_mode="geometry") can pull a source
        //           SOP node's cooked Geometry. Null in headless/smoke
        //           contexts where no SOP graph is active.
        struct DopEvalContext
        {
            SimState *state = nullptr;
            const DopGraph *graph = nullptr;
            const SopGeometryProvider *sopProvider = nullptr;
        };
    }
}
