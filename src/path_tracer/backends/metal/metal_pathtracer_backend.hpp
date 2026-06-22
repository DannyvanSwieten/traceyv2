// Metal ray tracing path tracer backend (macOS). Renders with Metal's
// inline intersector (hardware-accelerated on M3+, Apple's software
// intersector otherwise) into a VkImage whose backing MTLTexture is
// exported through VK_EXT_metal_objects — so the Vulkan viewport
// compositor blits the very image Metal wrote, zero copies.
//
// Pure C++ header: all Objective-C state lives behind the pimpl in the
// .mm so non-ObjC++ TUs (the registry) can include this freely.

#pragma once

#include "path_tracer/api/path_tracer_backend.hpp"

#include <memory>

namespace tracey
{
    class MetalPathTracerBackend : public PathTracerBackend
    {
    public:
        MetalPathTracerBackend();
        ~MetalPathTracerBackend() override;

        void initialize(const InitParams &params) override;
        PathTracerOutputKind outputKind() const override
        {
            return PathTracerOutputKind::BackendImage;
        }
        Image2D *backendOutputImage() override;
        void uploadMaterialPrograms(const MaterialProgramBuffer &programs) override;
        void uploadMaterialParameters(const MaterialProgramBuffer &programs) override;
        double dispatch(const SceneCompiler::CompiledScene &scene,
                        uint32_t accumulatedSampleCount,
                        bool clearAccumulation,
                        bool wantReadback) override;
        size_t readback(void *dst) override;
        bool aovsAvailable() const override;
        size_t readbackAOV(AovKind aov, void *dst) override;
        bool denoise() override;  // on-device OIDN Metal denoise → outputTexture

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // Registry hooks (defined in the .mm). Supported when the device is a
    // MoltenVK VulkanComputeDevice with VK_EXT_metal_objects and the system
    // Metal device supports ray tracing.
    bool metalRTBackendSupported(Device *device);
    std::unique_ptr<PathTracerBackend> createMetalRTBackend();
} // namespace tracey
