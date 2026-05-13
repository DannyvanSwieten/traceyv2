#pragma once

#include "../geometry/geometry.hpp"

#include <cstdint>

namespace tracey
{
    namespace dops
    {
        // Per-frame simulation header. The DopGraph fills this in before each
        // cookFrame() and passes it to every node via DopEvalContext, so
        // sources / solvers / forces don't each need to look up the timeline
        // independently.
        //
        //   frame      — 1-based frame counter (matches the timeline UI).
        //   time       — wall time in seconds: (frame - 1) / fps.
        //   dt         — per-substep delta = (1 / fps) / substepsPerFrame.
        //   substepIdx — which substep within the current frame, [0..N).
        //   substepsPerFrame — N. Solver decides; Phase 0 leaves it at 1.
        //
        // Phase 0 keeps everything single-object: a SimState owns one
        // Geometry. Multi-object sims (cross-actor RBD constraints, fluid
        // boundaries) graduate to a richer container later without touching
        // the per-node interface — only DopGraph's loop changes.
        struct SimHeader
        {
            int    frame = 1;
            double time = 0.0;
            double dt = 0.0;
            int    substepIdx = 0;
            int    substepsPerFrame = 1;
        };

        // The "state" that flows frame-to-frame through the DOP graph. For
        // particles this is just a Geometry whose points carry P / v / age /
        // life / id / force attributes. The header is rebuilt each frame from
        // the timeline; only the geometry persists across the frame boundary.
        struct SimState
        {
            Geometry geometry;
            SimHeader header;
        };
    }
}
