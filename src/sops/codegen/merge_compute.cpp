// GPU concat for the merge SOP. Pure vkCmdCopyBuffer — no shader, no
// descriptor set. The output attribute buffers are allocated through
// the usual Attribute<T>::buffer() path (which uploads the zero
// defaults from CPU as a side effect; cheap on Apple Silicon UMA but
// not free elsewhere — that's a follow-up optimisation when we add
// an "allocate-without-upload" Attribute API). Two vkCmdCopyBuffer
// calls per attribute then overwrite the zero region with the real
// data from the two source buffers.
//
// See merge_compute.hpp for the scope envelope.

#include "merge_compute.hpp"

#include "../../core/types.hpp"
#include "../../device/buffer.hpp"
#include "../../device/device.hpp"
#include "../../device/gpu/vulkan_buffer.hpp"
#include "../../device/gpu/vulkan_compute_device.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"
#include "../../geometry/geometry.hpp"
#include "../../gpu/vulkan_queue_sync.hpp"

#include <volk.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace codegen
        {
            namespace
            {
                [[noreturn]] void vkThrow(const char *where, VkResult r)
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                        "[merge:compute] %s failed: VkResult=%d", where,
                        static_cast<int>(r));
                    throw std::runtime_error(buf);
                }
                void vkCheck(VkResult r, const char *where)
                {
                    if (r != VK_SUCCESS) vkThrow(where, r);
                }

                bool attrsAreSubsetOf(const AttributeTable &t,
                                      const std::vector<const char *> &whitelist)
                {
                    for (const auto &name : t.names())
                    {
                        bool ok = false;
                        for (const char *w : whitelist)
                        {
                            if (name == w) { ok = true; break; }
                        }
                        if (!ok) return false;
                    }
                    return true;
                }

                bool isIdentityVtp(const std::vector<uint32_t> &vtp)
                {
                    for (size_t i = 0; i < vtp.size(); ++i)
                    {
                        if (vtp[i] != static_cast<uint32_t>(i)) return false;
                    }
                    return true;
                }

                std::atomic<MergeCompute *> g_dispatcher{nullptr};
                // Whole-dispatch lock lives in vulkanQueueMutex.

                // Record a `src1 → dst[0..bytes1)` + `src2 → dst[bytes1..bytes1+bytes2)`
                // pair into `cmd`. The dst buffer must already be allocated
                // and sized to bytes1 + bytes2.
                void recordConcatCopy(VkCommandBuffer cmd,
                                      VkBuffer src1, size_t bytes1,
                                      VkBuffer src2, size_t bytes2,
                                      VkBuffer dst)
                {
                    if (bytes1 > 0)
                    {
                        VkBufferCopy r{};
                        r.srcOffset = 0;
                        r.dstOffset = 0;
                        r.size = bytes1;
                        vkCmdCopyBuffer(cmd, src1, dst, 1, &r);
                    }
                    if (bytes2 > 0)
                    {
                        VkBufferCopy r{};
                        r.srcOffset = 0;
                        r.dstOffset = bytes1;
                        r.size = bytes2;
                        vkCmdCopyBuffer(cmd, src2, dst, 1, &r);
                    }
                }
            }

            MergeCompute *MergeCompute::getGlobal()
            {
                return g_dispatcher.load(std::memory_order_acquire);
            }
            void MergeCompute::setGlobal(MergeCompute *d)
            {
                g_dispatcher.store(d, std::memory_order_release);
            }

            struct MergeCompute::Impl
            {
                VulkanComputeDevice *device = nullptr;
            };

            MergeCompute::MergeCompute(Device *device)
                : m_impl(new Impl)
            {
                m_impl->device = dynamic_cast<VulkanComputeDevice *>(device);
                if (!device) throw std::runtime_error("[merge:compute] device is null");
                if (!m_impl->device) throw std::runtime_error(
                    "[merge:compute] device is not a VulkanComputeDevice");
            }
            MergeCompute::~MergeCompute()
            {
                delete m_impl;
            }

            bool MergeCompute::dispatch(const Geometry &a, const Geometry &b,
                                         Geometry &outArg) noexcept
            {
                try
                {
                    // Scope checks. Stay narrower than what mergeFrom can
                    // do — we only handle the standard attribute set, but
                    // that covers every output coming out of copy_to_points
                    // / fromSceneObject / transform / VOP today.
                    if (!attrsAreSubsetOf(a.points(),     {"P", "N"})) return false;
                    if (!attrsAreSubsetOf(a.vertices(),   {"uv", "Cd"})) return false;
                    if (a.primitives().names().size() != 0)               return false;
                    if (!isIdentityVtp(a.vertexToPoint()))                return false;
                    if (!attrsAreSubsetOf(b.points(),     {"P", "N"})) return false;
                    if (!attrsAreSubsetOf(b.vertices(),   {"uv", "Cd"})) return false;
                    if (b.primitives().names().size() != 0)               return false;
                    if (!isIdentityVtp(b.vertexToPoint()))                return false;

                    // P must exist on both (it's mandatory anyway).
                    const auto *aP = a.points().get<Vec3>("P");
                    const auto *bP = b.points().get<Vec3>("P");
                    if (!aP || !bP) return false;

                    // Optional attribute mirroring: only merge the
                    // attribute if BOTH inputs carry it. Matches
                    // mergeFrom's "drop on either-side absence" rule:
                    // attributes present only on one side get lost
                    // because the destination must pre-declare them.
                    const bool hasN  = a.points().get<Vec3>("N")    && b.points().get<Vec3>("N");
                    const bool hasUV = a.vertices().get<Vec2>("uv") && b.vertices().get<Vec2>("uv");
                    const bool hasCd = a.vertices().get<Vec3>("Cd") && b.vertices().get<Vec3>("Cd");

                    const size_t Na = a.pointCount();
                    const size_t Nb = b.pointCount();
                    const size_t N  = Na + Nb;
                    const size_t Va = a.vertexCount();
                    const size_t Vb = b.vertexCount();
                    const size_t V  = Va + Vb;
                    if (N == 0) return false;

                    std::lock_guard<std::mutex> lock(vulkanQueueMutex());

                    // Build output Geometry CPU-side, then back the
                    // attribute payloads with GPU buffers.
                    Geometry out;
                    if (hasN)  out.points().add<Vec3>("N",  Vec3(0.0f, 1.0f, 0.0f));
                    if (hasUV) out.vertices().add<Vec2>("uv", Vec2(0.0f));
                    if (hasCd) out.vertices().add<Vec3>("Cd", Vec3(1.0f));

                    out.points().resize(N);
                    out.vertices().resize(V);

                    // Identity vertexToPoint for the merged output.
                    auto &outVtp = out.vertexToPoint();
                    outVtp.resize(V);
                    for (size_t i = 0; i < V; ++i)
                        outVtp[i] = static_cast<uint32_t>(i);

                    // Primitives concatenated with vertex-offset for b.
                    auto &outPrims = out.primitivesList();
                    const auto &aPrims = a.primitivesList();
                    const auto &bPrims = b.primitivesList();
                    outPrims.reserve(aPrims.size() + bPrims.size());
                    for (const auto &p : aPrims) outPrims.push_back(p);
                    for (const auto &p : bPrims)
                    {
                        outPrims.push_back({p.firstVertex + static_cast<uint32_t>(Va),
                                            p.vertexCount});
                    }
                    out.primitives().resize(outPrims.size());

                    // Resolve input buffers (bufferConst — read-only, no
                    // generation bump). bail on any missing buffer; that
                    // would indicate the upload path for an empty attribute
                    // returned null, which mergeFrom would skip too.
                    auto srcBuf = [](const AttributeBase *attr) -> const Buffer * {
                        return attr ? attr->bufferConst() : nullptr;
                    };
                    const Buffer *aPbuf  = srcBuf(aP);
                    const Buffer *bPbuf  = srcBuf(bP);
                    const Buffer *aNbuf  = hasN  ? srcBuf(a.points().get<Vec3>("N"))    : nullptr;
                    const Buffer *bNbuf  = hasN  ? srcBuf(b.points().get<Vec3>("N"))    : nullptr;
                    const Buffer *aUVbuf = hasUV ? srcBuf(a.vertices().get<Vec2>("uv")) : nullptr;
                    const Buffer *bUVbuf = hasUV ? srcBuf(b.vertices().get<Vec2>("uv")) : nullptr;
                    const Buffer *aCdbuf = hasCd ? srcBuf(a.vertices().get<Vec3>("Cd")) : nullptr;
                    const Buffer *bCdbuf = hasCd ? srcBuf(b.vertices().get<Vec3>("Cd")) : nullptr;
                    if (!aPbuf || !bPbuf) return false;
                    if (hasN  && (!aNbuf  || !bNbuf))  return false;
                    if (hasUV && (!aUVbuf || !bUVbuf)) return false;
                    if (hasCd && (!aCdbuf || !bCdbuf)) return false;

                    // Output buffer allocations. buffer() also flips side
                    // to Gpu and bumps generation so downstream consumers
                    // see the new GPU data via lazy sync.
                    auto outBuf = [](AttributeBase *attr) -> Buffer * {
                        return attr ? attr->buffer() : nullptr;
                    };
                    Buffer *oPbuf  = outBuf(out.points().get<Vec3>("P"));
                    Buffer *oNbuf  = hasN  ? outBuf(out.points().get<Vec3>("N"))    : nullptr;
                    Buffer *oUVbuf = hasUV ? outBuf(out.vertices().get<Vec2>("uv")) : nullptr;
                    Buffer *oCdbuf = hasCd ? outBuf(out.vertices().get<Vec3>("Cd")) : nullptr;
                    if (!oPbuf) return false;
                    if (hasN  && !oNbuf)  return false;
                    if (hasUV && !oUVbuf) return false;
                    if (hasCd && !oCdbuf) return false;

                    VkDevice dev = m_impl->device->vkDevice();

                    VkCommandBufferAllocateInfo cmdAlloc{};
                    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    cmdAlloc.commandPool = m_impl->device->commandPool();
                    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    cmdAlloc.commandBufferCount = 1;
                    VkCommandBuffer cmd = VK_NULL_HANDLE;
                    vkCheck(vkAllocateCommandBuffers(dev, &cmdAlloc, &cmd),
                            "vkAllocateCommandBuffers");

                    VkCommandBufferBeginInfo bi{};
                    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkCheck(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

                    auto vkBufOf = [](const Buffer *b) {
                        return static_cast<const VulkanBuffer *>(b)->vkBuffer();
                    };
                    constexpr size_t vec3Stride = 16;  // std430
                    constexpr size_t vec2Stride = 8;

                    recordConcatCopy(cmd,
                        vkBufOf(aPbuf), Na * vec3Stride,
                        vkBufOf(bPbuf), Nb * vec3Stride,
                        vkBufOf(oPbuf));
                    if (hasN)
                    {
                        recordConcatCopy(cmd,
                            vkBufOf(aNbuf), Na * vec3Stride,
                            vkBufOf(bNbuf), Nb * vec3Stride,
                            vkBufOf(oNbuf));
                    }
                    if (hasUV)
                    {
                        recordConcatCopy(cmd,
                            vkBufOf(aUVbuf), Va * vec2Stride,
                            vkBufOf(bUVbuf), Vb * vec2Stride,
                            vkBufOf(oUVbuf));
                    }
                    if (hasCd)
                    {
                        recordConcatCopy(cmd,
                            vkBufOf(aCdbuf), Va * vec3Stride,
                            vkBufOf(bCdbuf), Vb * vec3Stride,
                            vkBufOf(oCdbuf));
                    }

                    // Barrier so any consumer that maps these buffers
                    // CPU-side immediately afterwards observes the
                    // copies. Same shape as the compute backends.
                    VkMemoryBarrier mb{};
                    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 1, &mb, 0, nullptr, 0, nullptr);

                    vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

                    VkFenceCreateInfo fci{};
                    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                    VkFence fence = VK_NULL_HANDLE;
                    vkCheck(vkCreateFence(dev, &fci, nullptr, &fence), "vkCreateFence");

                    VkSubmitInfo si{};
                    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    si.commandBufferCount = 1;
                    si.pCommandBuffers = &cmd;
                    vkCheck(vkQueueSubmit(m_impl->device->computeQueue(), 1, &si, fence),
                            "vkQueueSubmit");
                    vkCheck(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX),
                            "vkWaitForFences");

                    vkDestroyFence(dev, fence, nullptr);
                    vkFreeCommandBuffers(dev, m_impl->device->commandPool(), 1, &cmd);

                    outArg = std::move(out);
                    return true;
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr,
                        "[merge:compute] GPU dispatch failed, CPU fallback: %s\n",
                        e.what());
                    return false;
                }
                catch (...) { return false; }
            }
        }
    }
}
