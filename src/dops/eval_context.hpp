#pragma once

#include "sim_state.hpp"

namespace tracey
{
    namespace dops
    {
        class DopGraph;

        // Carried through every DOP node's cookFrame() call.
        //
        //   state — the SimState being built up for the current (frame,
        //           substep). Nodes mutate it in place: pop_source appends
        //           points, pop_gravity accumulates into `force`, pop_solver
        //           integrates v and P. Always non-null inside a cook.
        //   graph — the DopGraph driving the cook. Used by nodes that need
        //           to introspect (rare in Phase 0 — kept for parity with
        //           VopGraph's EvalContext shape).
        struct DopEvalContext
        {
            SimState *state = nullptr;
            const DopGraph *graph = nullptr;
        };
    }
}
