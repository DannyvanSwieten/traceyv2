// Metal RT path tracer backend — see header. Current state: SKELETON.
// The full Metal RT pipeline (acceleration structures, material VM, NEE)
// lands in subsequent steps; today's dispatch writes a placeholder
// gradient so the whole output path (Metal kernel → exported MTLTexture →
// Vulkan compositor blit / readback) can be exercised end to end with
// `TRACEY_PT_BACKEND=metal`.

#import <Metal/Metal.h>

#include "metal_pathtracer_backend.hpp"

#include "path_tracer/api/path_tracer.hpp"
#include "device/gpu/vulkan_compute_device.hpp"
#include "device/gpu/vulkan_image_2d.hpp"
#include "gpu/vulkan_context.hpp"
#include "gpu/vulkan_queue_sync.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tracey
{
    namespace
    {
        // Placeholder kernel: uv gradient + sample-count tint so editor-side
        // accumulation plumbing is visible while the real path tracer is
        // being ported. Replaced by the pathtrace megakernel.
        const char *kPlaceholderKernel = R"(
#include <metal_stdlib>
using namespace metal;

struct FrameParams {
    uint currentSample;
};

kernel void placeholder(texture2d<float, access::write> outImage [[texture(0)]],
                        constant FrameParams &frame [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]])
{
    const uint w = outImage.get_width();
    const uint h = outImage.get_height();
    if (gid.x >= w || gid.y >= h) return;
    const float2 uv = float2(gid) / float2(w, h);
    const float pulse = 0.5 + 0.5 * sin(float(frame.currentSample) * 0.05);
    outImage.write(float4(uv.x, uv.y, pulse, 1.0), gid);
}
)";
    }

    struct MetalPathTracerBackend::Impl
    {
        // Façade-owned state captured at initialize().
        VulkanComputeDevice *vkDevice = nullptr;
        const PathTracerConfig *config = nullptr;

        // The presentable output: a VulkanImage2D whose MTLTexture we own a
        // reference to. The compositor blits the VkImage; we write the
        // MTLTexture.
        std::unique_ptr<VulkanImage2D> outputImage;
        id<MTLTexture> outputTexture = nil;

        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<MTLComputePipelineState> placeholderPipeline = nil;

        // Shared-storage readback target for the export path.
        id<MTLBuffer> readbackBuffer = nil;
        size_t readbackBytes = 0;
    };

    MetalPathTracerBackend::MetalPathTracerBackend() : m_impl(std::make_unique<Impl>()) {}
    MetalPathTracerBackend::~MetalPathTracerBackend() = default;

    void MetalPathTracerBackend::initialize(const InitParams &params)
    {
        auto &impl = *m_impl;
        impl.vkDevice = dynamic_cast<VulkanComputeDevice *>(params.device);
        impl.config = params.config;
        if (!impl.vkDevice)
        {
            throw std::runtime_error("MetalRT backend requires a Vulkan compute device (MoltenVK)");
        }
        if (!impl.vkDevice->context().hasMetalObjectsExtension())
        {
            throw std::runtime_error("MetalRT backend requires VK_EXT_metal_objects");
        }

        // Output image: Vulkan-created so the compositor can blit it, with
        // the backing MTLTexture exported for our kernels to write.
        const ImageFormat format = impl.config->hdrOutput
                                       ? ImageFormat::R32G32B32A32Sfloat
                                       : ImageFormat::R8G8B8A8Unorm;
        impl.outputImage = std::make_unique<VulkanImage2D>(
            *impl.vkDevice, impl.config->width, impl.config->height, format,
            /*exportMetalTexture=*/true);
        impl.outputTexture = (__bridge id<MTLTexture>)impl.outputImage->exportedMetalTexture();
        if (!impl.outputTexture)
        {
            throw std::runtime_error("MetalRT backend: MTLTexture export failed");
        }

        impl.device = impl.outputTexture.device;
        if (!impl.device.supportsRaytracing)
        {
            throw std::runtime_error("MetalRT backend: this GPU has no Metal ray tracing support");
        }
        impl.queue = [impl.device newCommandQueue];

        NSError *error = nil;
        id<MTLLibrary> lib = [impl.device newLibraryWithSource:@(kPlaceholderKernel)
                                                       options:nil
                                                         error:&error];
        if (!lib)
        {
            throw std::runtime_error(std::string("MetalRT shader compile failed: ") +
                                     error.localizedDescription.UTF8String);
        }
        id<MTLFunction> fn = [lib newFunctionWithName:@"placeholder"];
        impl.placeholderPipeline = [impl.device newComputePipelineStateWithFunction:fn error:&error];
        if (!impl.placeholderPipeline)
        {
            throw std::runtime_error(std::string("MetalRT pipeline creation failed: ") +
                                     error.localizedDescription.UTF8String);
        }

        const size_t pixelSize = impl.config->hdrOutput ? 16 : 4;
        impl.readbackBytes =
            static_cast<size_t>(impl.config->width) * impl.config->height * pixelSize;
        impl.readbackBuffer = [impl.device newBufferWithLength:impl.readbackBytes
                                                       options:MTLResourceStorageModeShared];
    }

    Image2D *MetalPathTracerBackend::backendOutputImage()
    {
        return m_impl->outputImage.get();
    }

    void MetalPathTracerBackend::uploadMaterialPrograms(const MaterialProgramBuffer & /*programs*/)
    {
        // Skeleton: programs are consumed once the material VM kernel lands.
    }

    void MetalPathTracerBackend::uploadMaterialParameters(const MaterialProgramBuffer & /*programs*/)
    {
    }

    double MetalPathTracerBackend::dispatch(const SceneCompiler::CompiledScene & /*scene*/,
                                            uint32_t accumulatedSampleCount,
                                            bool /*clearAccumulation*/,
                                            bool wantReadback)
    {
        auto &impl = *m_impl;
        @autoreleasepool
        {
            // Keep the process-wide GPU submission order: the compositor's
            // blit of our output image is recorded after dispatch() returns,
            // and we CPU-wait below, so its reads always see a complete
            // frame. (If dispatch ever goes async, bridge a MTLSharedEvent
            // to a VkSemaphore via VK_EXT_metal_objects instead.)
            std::lock_guard<std::mutex> gpuLock(vulkanQueueMutex());

            id<MTLCommandBuffer> cmd = [impl.queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:impl.placeholderPipeline];
            [enc setTexture:impl.outputTexture atIndex:0];
            struct
            {
                uint32_t currentSample;
            } frame{accumulatedSampleCount + 1};
            [enc setBytes:&frame length:sizeof(frame) atIndex:0];
            const MTLSize tg = MTLSizeMake(8, 8, 1);
            const MTLSize grid = MTLSizeMake((impl.config->width + 7) / 8,
                                             (impl.config->height + 7) / 8, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];

            if (wantReadback)
            {
                const size_t pixelSize = impl.config->hdrOutput ? 16 : 4;
                id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
                [blit copyFromTexture:impl.outputTexture
                              sourceSlice:0
                              sourceLevel:0
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(impl.config->width, impl.config->height, 1)
                                 toBuffer:impl.readbackBuffer
                        destinationOffset:0
                   destinationBytesPerRow:impl.config->width * pixelSize
                 destinationBytesPerImage:impl.readbackBytes];
                [blit endEncoding];
            }

            const auto start = std::chrono::high_resolution_clock::now();
            [cmd commit];
            [cmd waitUntilCompleted];
            const auto end = std::chrono::high_resolution_clock::now();
            if (cmd.status == MTLCommandBufferStatusError)
            {
                throw std::runtime_error(std::string("MetalRT dispatch failed: ") +
                                         cmd.error.localizedDescription.UTF8String);
            }
            return std::chrono::duration<double, std::milli>(end - start).count();
        }
    }

    size_t MetalPathTracerBackend::readback(void *dst)
    {
        auto &impl = *m_impl;
        std::memcpy(dst, impl.readbackBuffer.contents, impl.readbackBytes);
        return impl.readbackBytes;
    }

    bool metalRTBackendSupported(Device *device)
    {
        auto *vk = dynamic_cast<VulkanComputeDevice *>(device);
        if (!vk || !vk->context().hasMetalObjectsExtension()) return false;
        @autoreleasepool
        {
            id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
            return mtl != nil && mtl.supportsRaytracing;
        }
    }

    std::unique_ptr<PathTracerBackend> createMetalRTBackend()
    {
        return std::make_unique<MetalPathTracerBackend>();
    }
} // namespace tracey
