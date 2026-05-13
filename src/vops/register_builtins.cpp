#include "register_builtins.hpp"

namespace tracey
{
    namespace vops
    {
        // Forward declarations of per-TU registration helpers. Each node
        // file under src/vops/nodes/ defines one of these so the static lib
        // doesn't drop the TU.
        void registerGeoIoVops();
        void registerMathVops();
        void registerNoiseVops();
        void registerDisplacementVops();

        void registerBuiltinVops()
        {
            registerGeoIoVops();
            registerMathVops();
            registerNoiseVops();
            registerDisplacementVops();
        }
    }
}
