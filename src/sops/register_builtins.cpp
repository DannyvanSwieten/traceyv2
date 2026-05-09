#include "sop_registry.hpp"

namespace tracey
{
    namespace sops
    {
        // Per-TU registration entry points. Each lives in the corresponding
        // node .cpp; declared here so this aggregator pulls those TUs into
        // the static-lib link (they have no other externally-referenced
        // symbol).
        void registerPrimitiveSops();
        void registerTransformSop();
        void registerMergeSop();
        void registerObjectOutputSop();
        void registerGltfImportSop();

        void registerBuiltinSops()
        {
            // Categories run in this order for the palette UI:
            //   Generators → Modifiers → Combiners → Output
            registerPrimitiveSops();
            registerGltfImportSop();
            registerTransformSop();
            registerMergeSop();
            registerObjectOutputSop();
        }
    }
}
