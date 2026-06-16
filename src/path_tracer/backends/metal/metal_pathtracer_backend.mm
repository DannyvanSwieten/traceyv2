// Metal RT path tracer backend — see header.
//
// Renders with Metal's inline intersector via the megakernel in
// pathtrace_msl.hpp (a line-by-line port of the canonical GLSL set the
// wavefront renderer runs). Scene resources are (re)built whenever the
// CompiledScene's revision stamp changes:
//   • one MTLPrimitiveAccelerationStructure per unique vertex buffer,
//     cached across revisions by Buffer* identity (mirrors BlasCache),
//   • the MTLInstanceAccelerationStructure + per-instance buffers
//     (programId/uvOffset pairs, inverse-transpose normal matrices),
//   • CPU-side scene arrays (lights, per-instance materials, UVs, normals,
//     concatenated positions) copied into MTLStorageModeShared buffers,
//   • MTLTextures created from CompiledScene::textureSources.
//
// The output image is a façade-visible VulkanImage2D whose backing
// MTLTexture is exported through VK_EXT_metal_objects — the Vulkan
// compositor blits the very image these kernels write. dispatch() waits
// for command-buffer completion before returning (same contract as the
// wavefront backend), which is what keeps the cross-API handoff safe.

#import <Metal/Metal.h>

#include "metal_pathtracer_backend.hpp"
#include "pathtrace_msl.hpp"

#include "path_tracer/api/path_tracer.hpp"
#include "device/gpu/vulkan_compute_device.hpp"
#include "device/gpu/vulkan_image_2d.hpp"
#include "gpu/vulkan_context.hpp"
#include "gpu/vulkan_queue_sync.hpp"

#include <glm/glm.hpp>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace tracey
{
    namespace
    {
        // ShaderInputs std140 offsets — must match the member order declared
        // in PathTracer::createShaderInputs() (fov, cameraPosition, forward,
        // right, up, maxDepth, currentSample, lightCount).
        struct ShaderInputsView
        {
            float fov;
            glm::vec3 cameraPosition;
            glm::vec3 cameraForward;
            glm::vec3 cameraRight;
            glm::vec3 cameraUp;
            uint32_t maxDepth;
            int32_t currentSample;
            uint32_t lightCount;
        };

        ShaderInputsView readShaderInputs(const ShaderInputsBuffer &inputs)
        {
            const auto *bytes = static_cast<const uint8_t *>(inputs.buffer()->mapForReading());
            auto readVec3 = [&](size_t off) {
                glm::vec3 v;
                std::memcpy(&v, bytes + off, sizeof(v));
                return v;
            };
            ShaderInputsView out;
            std::memcpy(&out.fov, bytes + 0, sizeof(float));
            out.cameraPosition = readVec3(16);
            out.cameraForward = readVec3(32);
            out.cameraRight = readVec3(48);
            out.cameraUp = readVec3(64);
            std::memcpy(&out.maxDepth, bytes + 80, sizeof(uint32_t));
            std::memcpy(&out.currentSample, bytes + 84, sizeof(int32_t));
            std::memcpy(&out.lightCount, bytes + 88, sizeof(uint32_t));
            inputs.buffer()->unmap();
            return out;
        }

        // Uniforms block consumed by the kernel — layout must match the MSL
        // `Uniforms` struct (float3 members are 16-byte aligned in MSL
        // constant address space).
        struct alignas(16) KernelUniforms
        {
            float camPos[3];
            float _pad0;
            float camFwd[3];
            float _pad1;
            float camRight[3];
            float _pad2;
            float camUp[3];
            float _pad3;
            float fovDegrees;
            uint32_t maxDepth;
            int32_t currentSample;
            uint32_t lightCount;
            uint32_t samplesPerFrame;
            uint32_t width;
            uint32_t height;
            uint32_t emitterCount;  // was _pad4 — emissive-triangle count for NEE
        };

        id<MTLBuffer> makeSharedBuffer(id<MTLDevice> device, const void *data, size_t bytes)
        {
            if (bytes == 0)
            {
                // MSL device pointers must be bound to something; 16 zero
                // bytes covers every element type we bind.
                static const uint8_t zeros[16] = {};
                return [device newBufferWithBytes:zeros length:sizeof(zeros)
                                          options:MTLResourceStorageModeShared];
            }
            return [device newBufferWithBytes:data length:bytes
                                      options:MTLResourceStorageModeShared];
        }
    }

    struct MetalPathTracerBackend::Impl
    {
        VulkanComputeDevice *vkDevice = nullptr;
        const PathTracerConfig *config = nullptr;
        ShaderInputsBuffer *shaderInputs = nullptr;

        // Output: Vulkan image the compositor blits; Metal texture we write.
        std::unique_ptr<VulkanImage2D> outputImage;
        id<MTLTexture> outputTexture = nil;

        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<MTLComputePipelineState> pathtracePipeline = nil;

        // Accumulator: float4 per pixel, running mean (resolve.glsl).
        id<MTLBuffer> accumBuffer = nil;

        // Readback target (wantReadback / export path).
        id<MTLBuffer> readbackBuffer = nil;
        size_t readbackBytes = 0;

        // ── Per-scene resources, rebuilt when `sceneRevision` changes ──
        uint64_t sceneRevision = ~0ull;

        struct BlasEntry
        {
            id<MTLAccelerationStructure> accel = nil;
            id<MTLBuffer> positions = nil;
            uint32_t vertexCount = 0;
            bool touched = false;
        };
        std::unordered_map<const Buffer *, BlasEntry> blasCache;

        id<MTLAccelerationStructure> instanceAccel = nil;
        id<MTLBuffer> instanceDescriptors = nil;
        NSMutableArray<id<MTLAccelerationStructure>> *primitiveList = nil;

        id<MTLBuffer> lightsBuf = nil;
        id<MTLBuffer> emittersBuf = nil;   // emissive-triangle list (4 float4/tri)
        uint32_t emitterCount = 0;
        id<MTLBuffer> materialsBuf = nil;
        id<MTLBuffer> instanceDataBuf = nil;
        id<MTLBuffer> uvsBuf = nil;
        id<MTLBuffer> normalsBuf = nil;
        id<MTLBuffer> positionsBuf = nil;     // concatenated, BLAS order
        id<MTLBuffer> normalMatsBuf = nil;    // 3 float4 rows per instance
        NSMutableArray<id<MTLTexture>> *sceneTextures = nil;
        id<MTLTexture> dummyTexture = nil;

        // ── Material program buffers (uploadMaterialPrograms) ──
        id<MTLBuffer> progCodeBuf = nil;
        id<MTLBuffer> progConstBuf = nil;
        id<MTLBuffer> progHeadersBuf = nil;
        id<MTLBuffer> progParamsBuf = nil;

        // ── helpers ──────────────────────────────────────────────────────

        id<MTLAccelerationStructure> buildAccelerationStructure(
            MTLAccelerationStructureDescriptor *descriptor)
        {
            MTLAccelerationStructureSizes sizes =
                [device accelerationStructureSizesWithDescriptor:descriptor];
            id<MTLAccelerationStructure> accel =
                [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
            id<MTLBuffer> scratch =
                [device newBufferWithLength:sizes.buildScratchBufferSize
                                    options:MTLResourceStorageModePrivate];
            id<MTLCommandBuffer> cmd = [queue commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc =
                [cmd accelerationStructureCommandEncoder];
            [enc buildAccelerationStructure:accel
                                 descriptor:descriptor
                              scratchBuffer:scratch
                        scratchBufferOffset:0];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            if (cmd.status == MTLCommandBufferStatusError)
            {
                throw std::runtime_error(
                    std::string("MetalRT: acceleration structure build failed: ") +
                    cmd.error.localizedDescription.UTF8String);
            }
            return accel;
        }

        BlasEntry &primitiveAccelFor(const Buffer *vertexBuffer, uint32_t vertexCount)
        {
            auto it = blasCache.find(vertexBuffer);
            if (it != blasCache.end() && it->second.vertexCount == vertexCount)
            {
                it->second.touched = true;
                return it->second;
            }

            BlasEntry entry;
            entry.vertexCount = vertexCount;
            entry.touched = true;
            // Triangle soup: packed Vec3 positions, 12-byte stride, no index
            // buffer (matches the engine's vertex buffer layout).
            entry.positions = makeSharedBuffer(device, vertexBuffer->mapForReading(),
                                               static_cast<size_t>(vertexCount) * 12);
            vertexBuffer->unmap();

            MTLAccelerationStructureTriangleGeometryDescriptor *geo =
                [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            geo.vertexBuffer = entry.positions;
            geo.vertexBufferOffset = 0;
            geo.vertexStride = 12;
            geo.vertexFormat = MTLAttributeFormatFloat3;
            geo.triangleCount = vertexCount / 3;
            geo.indexBuffer = nil;
            geo.opaque = YES;

            MTLPrimitiveAccelerationStructureDescriptor *desc =
                [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            desc.geometryDescriptors = @[ geo ];

            entry.accel = buildAccelerationStructure(desc);
            return blasCache[vertexBuffer] = std::move(entry);
        }

        void bindScene(const SceneCompiler::CompiledScene &scene)
        {
            if (scene.revision == sceneRevision) return;
            sceneRevision = scene.revision;

            // ── Primitive acceleration structures (mark/sweep cache) ──
            for (auto &kv : blasCache) kv.second.touched = false;

            primitiveList = [NSMutableArray array];
            std::vector<uint32_t> blasVertexBase(scene.vertexBuffers.size(), 0);
            uint32_t totalVertices = 0;
            for (size_t i = 0; i < scene.vertexBuffers.size(); ++i)
            {
                blasVertexBase[i] = totalVertices;
                totalVertices += scene.vertexCounts[i];
                BlasEntry &entry = primitiveAccelFor(scene.vertexBuffers[i], scene.vertexCounts[i]);
                [primitiveList addObject:entry.accel];
            }
            for (auto it = blasCache.begin(); it != blasCache.end();)
            {
                if (!it->second.touched) it = blasCache.erase(it);
                else ++it;
            }

            // ── Concatenated positions (BLAS order — same addressing as the
            //    global UV/normal arrays via instanceData.y) ──
            {
                std::vector<float> all(static_cast<size_t>(totalVertices) * 3, 0.0f);
                for (size_t i = 0; i < scene.vertexBuffers.size(); ++i)
                {
                    const auto *src = static_cast<const float *>(scene.vertexBuffers[i]->mapForReading());
                    std::memcpy(all.data() + static_cast<size_t>(blasVertexBase[i]) * 3, src,
                                static_cast<size_t>(scene.vertexCounts[i]) * 12);
                    scene.vertexBuffers[i]->unmap();
                }
                positionsBuf = makeSharedBuffer(device, all.data(), all.size() * sizeof(float));
            }

            // ── Instance acceleration structure ──
            const size_t instanceCount = scene.instances.size();
            {
                std::vector<MTLAccelerationStructureInstanceDescriptor> descs(
                    std::max<size_t>(instanceCount, 1));
                for (size_t i = 0; i < instanceCount; ++i)
                {
                    const Tlas::Instance &inst = scene.instances[i];
                    MTLAccelerationStructureInstanceDescriptor &d = descs[i];
                    // Row-major 3×4 → MTLPackedFloat4x3 (4 columns of float3).
                    for (int c = 0; c < 4; ++c)
                    {
                        d.transformationMatrix.columns[c] = MTLPackedFloat3Make(
                            inst.transform[0][c], inst.transform[1][c], inst.transform[2][c]);
                    }
                    d.options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling |
                                MTLAccelerationStructureInstanceOptionOpaque;
                    d.mask = 0xFFu;
                    d.intersectionFunctionTableOffset = 0;
                    d.accelerationStructureIndex = static_cast<uint32_t>(inst.blasAddress);
                }
                if (instanceCount == 0)
                {
                    // Metal requires ≥1 instance; park a degenerate one far
                    // away (the editor skips empty scenes anyway).
                    MTLAccelerationStructureInstanceDescriptor &d = descs[0];
                    for (int c = 0; c < 4; ++c)
                        d.transformationMatrix.columns[c] =
                            MTLPackedFloat3Make(c == 0 ? 1 : 0, c == 1 ? 1 : 0, c == 2 ? 1 : 0);
                    d.transformationMatrix.columns[3] = MTLPackedFloat3Make(1e7f, 1e7f, 1e7f);
                    d.options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
                    d.mask = 0;
                    d.intersectionFunctionTableOffset = 0;
                    d.accelerationStructureIndex = 0;
                }
                instanceDescriptors = makeSharedBuffer(
                    device, descs.data(),
                    descs.size() * sizeof(MTLAccelerationStructureInstanceDescriptor));

                if (primitiveList.count == 0)
                {
                    // Degenerate single-triangle BLAS so the instance AS has
                    // something to reference.
                    const float tri[9] = {1e7f, 1e7f, 1e7f, 1e7f + 1, 1e7f, 1e7f, 1e7f, 1e7f + 1, 1e7f};
                    id<MTLBuffer> buf = makeSharedBuffer(device, tri, sizeof(tri));
                    MTLAccelerationStructureTriangleGeometryDescriptor *geo =
                        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
                    geo.vertexBuffer = buf;
                    geo.vertexStride = 12;
                    geo.vertexFormat = MTLAttributeFormatFloat3;
                    geo.triangleCount = 1;
                    geo.opaque = YES;
                    MTLPrimitiveAccelerationStructureDescriptor *pdesc =
                        [MTLPrimitiveAccelerationStructureDescriptor descriptor];
                    pdesc.geometryDescriptors = @[ geo ];
                    [primitiveList addObject:buildAccelerationStructure(pdesc)];
                }

                MTLInstanceAccelerationStructureDescriptor *idesc =
                    [MTLInstanceAccelerationStructureDescriptor descriptor];
                idesc.instancedAccelerationStructures = primitiveList;
                idesc.instanceCount = std::max<size_t>(instanceCount, 1);
                idesc.instanceDescriptorBuffer = instanceDescriptors;
                idesc.instanceDescriptorBufferOffset = 0;
                idesc.instanceDescriptorStride = sizeof(MTLAccelerationStructureInstanceDescriptor);
                instanceAccel = buildAccelerationStructure(idesc);
            }

            // ── Per-instance shading data ──
            {
                struct Pair { uint32_t programId; uint32_t uvOffset; };
                std::vector<Pair> pairs(std::max<size_t>(instanceCount, 1), Pair{0, 0});
                for (size_t i = 0; i < instanceCount; ++i)
                {
                    pairs[i].programId = i < scene.instanceProgramIndex.size()
                                             ? scene.instanceProgramIndex[i] : 0u;
                    pairs[i].uvOffset = i < scene.instanceUvOffset.size()
                                            ? scene.instanceUvOffset[i] : 0u;
                }
                instanceDataBuf = makeSharedBuffer(device, pairs.data(),
                                                   pairs.size() * sizeof(Pair));

                // Inverse-transpose 3×3 per instance, stored as 3 float4 rows.
                std::vector<float> nm(std::max<size_t>(instanceCount, 1) * 12, 0.0f);
                for (size_t i = 0; i < instanceCount; ++i)
                {
                    const auto &t = scene.instances[i].transform;  // [row][col]
                    glm::mat3 A;  // glm is column-major: A[col][row]
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            A[c][r] = t[r][c];
                    const glm::mat3 NM = glm::transpose(glm::inverse(A));
                    for (int r = 0; r < 3; ++r)
                    {
                        nm[i * 12 + r * 4 + 0] = NM[0][r];
                        nm[i * 12 + r * 4 + 1] = NM[1][r];
                        nm[i * 12 + r * 4 + 2] = NM[2][r];
                        nm[i * 12 + r * 4 + 3] = 0.0f;
                    }
                }
                normalMatsBuf = makeSharedBuffer(device, nm.data(), nm.size() * sizeof(float));
            }

            // ── Scene arrays ──
            lightsBuf = makeSharedBuffer(device, scene.lights.data(),
                                         scene.lights.size() * sizeof(GPULight));
            materialsBuf = makeSharedBuffer(device, scene.materials.data(),
                                            scene.materials.size() * sizeof(GPUMaterial));

            // Emissive triangles → 4 float4 per tri: p0(xyz)+area, p1, p2,
            // emission. Empty list → makeSharedBuffer binds a 16-byte dummy.
            emitterCount = static_cast<uint32_t>(scene.emitters.size());
            {
                std::vector<float> packed(scene.emitters.size() * 16);
                for (size_t i = 0; i < scene.emitters.size(); ++i) {
                    const auto &e = scene.emitters[i];
                    float *d = packed.data() + i * 16;
                    d[0] = e.p0.x; d[1] = e.p0.y; d[2] = e.p0.z; d[3] = e.area;
                    d[4] = e.p1.x; d[5] = e.p1.y; d[6] = e.p1.z; d[7] = 0.0f;
                    d[8] = e.p2.x; d[9] = e.p2.y; d[10] = e.p2.z; d[11] = 0.0f;
                    d[12] = e.emission.x; d[13] = e.emission.y; d[14] = e.emission.z; d[15] = 0.0f;
                }
                emittersBuf = makeSharedBuffer(device, packed.data(),
                                               packed.size() * sizeof(float));
            }

            const size_t uvBytes = static_cast<size_t>(totalVertices) * sizeof(float) * 2;
            if (scene.uvBuffer && uvBytes > 0)
            {
                uvsBuf = makeSharedBuffer(device, scene.uvBuffer->mapForReading(), uvBytes);
                scene.uvBuffer->unmap();
            }
            else
            {
                uvsBuf = makeSharedBuffer(device, nullptr, 0);
            }

            const size_t normalBytes = static_cast<size_t>(totalVertices) * sizeof(float) * 4;
            if (scene.normalBuffer && normalBytes > 0)
            {
                normalsBuf = makeSharedBuffer(device, scene.normalBuffer->mapForReading(), normalBytes);
                scene.normalBuffer->unmap();
            }
            else
            {
                // All-zero normals trigger the kernel's face-normal fallback.
                std::vector<float> zeros(std::max<size_t>(totalVertices, 1) * 4, 0.0f);
                normalsBuf = makeSharedBuffer(device, zeros.data(), zeros.size() * sizeof(float));
            }

            // ── Textures ──
            sceneTextures = [NSMutableArray array];
            for (const auto &src : scene.textureSources)
            {
                if (sceneTextures.count >= kMetalRTMaxTextures) break;
                MTLTextureDescriptor *td = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:(src.srgb ? MTLPixelFormatRGBA8Unorm_sRGB
                                                                 : MTLPixelFormatRGBA8Unorm)
                                                 width:src.width
                                                height:src.height
                                             mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead;
                id<MTLTexture> tex = [device newTextureWithDescriptor:td];
                [tex replaceRegion:MTLRegionMake2D(0, 0, src.width, src.height)
                       mipmapLevel:0
                         withBytes:src.rgba8.data()
                       bytesPerRow:static_cast<NSUInteger>(src.width) * 4];
                [sceneTextures addObject:tex];
            }
        }
    };

    MetalPathTracerBackend::MetalPathTracerBackend() : m_impl(std::make_unique<Impl>()) {}
    MetalPathTracerBackend::~MetalPathTracerBackend() = default;

    void MetalPathTracerBackend::initialize(const InitParams &params)
    {
        auto &impl = *m_impl;
        impl.vkDevice = dynamic_cast<VulkanComputeDevice *>(params.device);
        impl.config = params.config;
        impl.shaderInputs = params.shaderInputs;
        if (!impl.vkDevice)
        {
            throw std::runtime_error("MetalRT backend requires a Vulkan compute device (MoltenVK)");
        }
        if (!impl.vkDevice->context().hasMetalObjectsExtension())
        {
            throw std::runtime_error("MetalRT backend requires VK_EXT_metal_objects");
        }

        // Output image: Vulkan-created so the compositor can blit it, with
        // the backing MTLTexture exported for our kernel to write.
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
        MTLCompileOptions *opts = [MTLCompileOptions new];
        id<MTLLibrary> lib = [impl.device newLibraryWithSource:@(kPathtraceMSL)
                                                       options:opts
                                                         error:&error];
        if (!lib)
        {
            throw std::runtime_error(std::string("MetalRT shader compile failed: ") +
                                     error.localizedDescription.UTF8String);
        }
        id<MTLFunction> fn = [lib newFunctionWithName:@"pathtrace"];
        impl.pathtracePipeline = [impl.device newComputePipelineStateWithFunction:fn error:&error];
        if (!impl.pathtracePipeline)
        {
            throw std::runtime_error(std::string("MetalRT pipeline creation failed: ") +
                                     error.localizedDescription.UTF8String);
        }

        const size_t pixelCount = static_cast<size_t>(impl.config->width) * impl.config->height;
        impl.accumBuffer = [impl.device newBufferWithLength:pixelCount * 16
                                                    options:MTLResourceStorageModePrivate];

        const size_t pixelSize = impl.config->hdrOutput ? 16 : 4;
        impl.readbackBytes = pixelCount * pixelSize;
        impl.readbackBuffer = [impl.device newBufferWithLength:impl.readbackBytes
                                                       options:MTLResourceStorageModeShared];

        // 1×1 white dummy for unused texture-array slots.
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        impl.dummyTexture = [impl.device newTextureWithDescriptor:td];
        const uint8_t white[4] = {255, 255, 255, 255};
        [impl.dummyTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                             mipmapLevel:0
                               withBytes:white
                             bytesPerRow:4];

        // Program buffers start as dummies; uploadMaterialPrograms replaces.
        impl.progCodeBuf = makeSharedBuffer(impl.device, nullptr, 0);
        impl.progConstBuf = makeSharedBuffer(impl.device, nullptr, 0);
        impl.progHeadersBuf = makeSharedBuffer(impl.device, nullptr, 0);
        impl.progParamsBuf = makeSharedBuffer(impl.device, nullptr, 0);
    }

    Image2D *MetalPathTracerBackend::backendOutputImage()
    {
        return m_impl->outputImage.get();
    }

    void MetalPathTracerBackend::uploadMaterialPrograms(const MaterialProgramBuffer &programs)
    {
        auto &impl = *m_impl;
        impl.progCodeBuf = makeSharedBuffer(impl.device, programs.code().data(),
                                            programs.codeBytes());
        impl.progConstBuf = makeSharedBuffer(impl.device, programs.constants().data(),
                                             programs.constantsBytes());
        impl.progHeadersBuf = makeSharedBuffer(impl.device, programs.headers().data(),
                                               programs.headersBytes());
        impl.progParamsBuf = makeSharedBuffer(impl.device, programs.parameters().data(),
                                              programs.parametersBytes());
    }

    void MetalPathTracerBackend::uploadMaterialParameters(const MaterialProgramBuffer &programs)
    {
        auto &impl = *m_impl;
        if (impl.progParamsBuf.length >= programs.parametersBytes() &&
            programs.parametersBytes() > 0)
        {
            std::memcpy(impl.progParamsBuf.contents, programs.parameters().data(),
                        programs.parametersBytes());
        }
        else
        {
            impl.progParamsBuf = makeSharedBuffer(impl.device, programs.parameters().data(),
                                                  programs.parametersBytes());
        }
    }

    double MetalPathTracerBackend::dispatch(const SceneCompiler::CompiledScene &scene,
                                            uint32_t /*accumulatedSampleCount*/,
                                            bool clearAccumulation,
                                            bool wantReadback)
    {
        auto &impl = *m_impl;
        @autoreleasepool
        {
            // Preserve the process-wide GPU submission order: the
            // compositor's blit of our output image is recorded after
            // dispatch() returns and we CPU-wait below, so its reads always
            // see a complete frame.
            std::lock_guard<std::mutex> gpuLock(vulkanQueueMutex());

            impl.bindScene(scene);

            const ShaderInputsView inputs = readShaderInputs(*impl.shaderInputs);
            KernelUniforms U{};
            std::memcpy(U.camPos, &inputs.cameraPosition, 12);
            std::memcpy(U.camFwd, &inputs.cameraForward, 12);
            std::memcpy(U.camRight, &inputs.cameraRight, 12);
            std::memcpy(U.camUp, &inputs.cameraUp, 12);
            U.fovDegrees = inputs.fov;
            U.maxDepth = inputs.maxDepth;
            U.currentSample = inputs.currentSample;
            U.lightCount = inputs.lightCount;
            U.samplesPerFrame = impl.config->samplesPerFrame;
            U.width = impl.config->width;
            U.height = impl.config->height;
            U.emitterCount = impl.emitterCount;

            id<MTLCommandBuffer> cmd = [impl.queue commandBuffer];

            if (clearAccumulation)
            {
                id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
                [blit fillBuffer:impl.accumBuffer
                           range:NSMakeRange(0, impl.accumBuffer.length)
                           value:0];
                [blit endEncoding];
            }

            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:impl.pathtracePipeline];
            [enc setTexture:impl.outputTexture atIndex:0];
            // Texture array slots 1..kMax: scene textures then dummy fill.
            for (NSUInteger i = 0; i < kMetalRTMaxTextures; ++i)
            {
                id<MTLTexture> t = (impl.sceneTextures && i < impl.sceneTextures.count)
                                       ? impl.sceneTextures[i]
                                       : impl.dummyTexture;
                [enc setTexture:t atIndex:1 + i];
            }
            [enc setBytes:&U length:sizeof(U) atIndex:0];
            [enc setBuffer:impl.lightsBuf offset:0 atIndex:1];
            [enc setBuffer:impl.materialsBuf offset:0 atIndex:2];
            [enc setBuffer:impl.instanceDataBuf offset:0 atIndex:3];
            [enc setBuffer:impl.uvsBuf offset:0 atIndex:4];
            [enc setBuffer:impl.normalsBuf offset:0 atIndex:5];
            [enc setBuffer:impl.positionsBuf offset:0 atIndex:6];
            [enc setBuffer:impl.normalMatsBuf offset:0 atIndex:7];
            [enc setBuffer:impl.progCodeBuf offset:0 atIndex:8];
            [enc setBuffer:impl.progConstBuf offset:0 atIndex:9];
            [enc setBuffer:impl.progHeadersBuf offset:0 atIndex:10];
            [enc setBuffer:impl.progParamsBuf offset:0 atIndex:11];
            [enc setBuffer:impl.accumBuffer offset:0 atIndex:12];
            [enc setAccelerationStructure:impl.instanceAccel atBufferIndex:13];
            [enc setBuffer:impl.emittersBuf offset:0 atIndex:14];
            // The instance AS references the primitive ASes indirectly —
            // mark them resident for the dispatch.
            for (id<MTLAccelerationStructure> blas in impl.primitiveList)
            {
                [enc useResource:blas usage:MTLResourceUsageRead];
            }

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
