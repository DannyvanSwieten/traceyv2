#include "register_builtins.hpp"

namespace tracey
{
    namespace vops
    {
        // Forward declarations of per-TU registration helpers. Each node
        // file under src/vops/nodes/ defines one of these so the static lib
        // doesn't drop the TU. Filled in across Steps 3–4.
        void registerBindVops();
        void registerBindAttrVops();
        void registerMathVops();
        void registerNoiseVops();
        void registerDisplacementVops();

        void registerBuiltinVops()
        {
            registerBindVops();
            registerBindAttrVops();
            registerMathVops();
            registerNoiseVops();
            registerDisplacementVops();
        }
    }
}
