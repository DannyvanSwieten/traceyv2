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
        void registerInstanceSop();
        void registerInstanceVopSop();
        void registerScatterSop();
        void registerLightSop();
        void registerNormalSop();
        void registerDopImportSop();
        void registerDeleteSop();
        void registerSwitchSop();
        void registerBoundSop();
        void registerSortSop();
        void registerPlainEffectorSop();
        void registerRandomEffectorSop();
        void registerNoiseEffectorSop();
        void registerStepEffectorSop();
        void registerLineSop();
        void registerCircleSop();
        void registerSpiralSop();
        void registerResampleSop();
        void registerSweepSop();
        void registerMoTextSop();

        void registerBuiltinSops()
        {
            // Palette category order: Generators → Cloners → Modifiers →
            // Effectors → Combiners → Output → Lights → Subnet.
            registerPrimitiveSops();
            registerGltfImportSop();
            registerLineSop();
            registerCircleSop();
            registerSpiralSop();
            registerMoTextSop();
            registerPointsGridSop();
            registerScatterSop();
            registerCopyToPointsSop();
            registerInstanceSop();
            registerInstanceVopSop();
            registerTransformSop();
            registerNormalSop();
            registerAttributeVopSop();
            registerResampleSop();
            registerDeleteSop();
            registerSwitchSop();
            registerSortSop();
            registerBoundSop();
            registerPlainEffectorSop();
            registerRandomEffectorSop();
            registerNoiseEffectorSop();
            registerStepEffectorSop();
            registerSweepSop();
            registerMergeSop();
            registerObjectOutputSop();
            registerLightSop();
            registerSubnetSop();
            registerDopImportSop();
        }
    }
}
