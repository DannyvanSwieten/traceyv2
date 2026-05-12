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
        void registerSubnetSop();
        void registerAttributeVopSop();
        void registerPointsGridSop();
        void registerCopyToPointsSop();
        void registerScatterSop();
        void registerLightSop();

        void registerBuiltinSops()
        {
            // Palette category order: Generators → Cloners → Modifiers →
            // Combiners → Output → Lights → Subnet.
            registerPrimitiveSops();
            registerGltfImportSop();
            registerPointsGridSop();
            registerScatterSop();
            registerCopyToPointsSop();
            registerTransformSop();
            registerAttributeVopSop();
            registerMergeSop();
            registerObjectOutputSop();
            registerLightSop();
            registerSubnetSop();
        }
    }
}
