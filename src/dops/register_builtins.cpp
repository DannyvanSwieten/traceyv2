#include "register_builtins.hpp"

namespace tracey
{
    namespace dops
    {
        // Forward declarations of per-TU registration helpers. Each node
        // file under src/dops/nodes/ defines one of these so the static lib
        // doesn't drop the TU.
        void registerSourceDops();
        void registerForceDops();
        void registerSolverDops();
        void registerPopForceDop();
        void registerPopDragDop();
        void registerPopWindDop();
        void registerPopAttractDop();
        void registerPopSpeedLimitDop();
        void registerPopKillDop();

        void registerBuiltinDops()
        {
            // Catalog order is purely palette grouping — DOPs cook in
            // topo order at runtime regardless of registration order.
            registerSourceDops();
            registerForceDops();      // pop_gravity
            registerPopForceDop();    // pop_force (VOP-driven)
            registerPopDragDop();
            registerPopWindDop();
            registerPopAttractDop();
            registerSolverDops();     // pop_solver (integrator)
            registerPopSpeedLimitDop();
            registerPopKillDop();
        }
    }
}
