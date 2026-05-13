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

        void registerBuiltinDops()
        {
            registerSourceDops();
            registerForceDops();
            registerPopForceDop();
            registerSolverDops();
        }
    }
}
