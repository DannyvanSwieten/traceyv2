// VOP → Vulkan compute dispatcher. See compute_dispatch.hpp for the
// design rationale (Phase 2 of the GPU-VOP path; CPU evaluator stays
// the fallback). The dispatcher is intentionally self-contained — no
// generic compute-pipeline abstraction at the Device level yet.
//
// Implementation outline:
//   compileOrGet(graph):
//     - emitGlsl(graph) → GLSL + binding tables
//     - hashGlsl + cache lookup; hit → return entry
//     - miss → ShaderCompiler → VkShaderModule → descriptor-set layout
//       (one binding per touched attr + one for params) →
//       pipeline layout (with `uint pointCount` push constant) →
//       VkPipeline; stash entry in cache, return.
//   dispatch(graph, geo):
//     - compileOrGet pipeline
//     - allocate transient SSBOs (one per attr, plus params)
//     - upload inputs via host-coherent mapping (std430 vec3 → vec4
//       padding handled here)
//     - allocate VkDescriptorSet, write binding writes
//     - record + submit a one-shot command buffer; wait on fence
//     - read back writeback attrs into the live geometry attribute
//       arrays, unpacking vec4 → vec3 where needed
//
// Failure handling: throws std::runtime_error on any Vulkan / shaderc
// error; the caller should fall back to the CPU evaluator and
// continue.

#include "compute_dispatch.hpp"

#include "../../core/types.hpp"
#include "../../device/buffer.hpp"
#include "../../device/device.hpp"
#include "../../device/gpu/vulkan_buffer.hpp"
#include "../../device/gpu/vulkan_compute_device.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"
#include "../../geometry/geometry.hpp"
#include "../../gpu/shader_compiler.hpp"
#include "../../gpu/vulkan_queue_sync.hpp"
#include "../geo_io_ports.hpp"
#include "glsl_emit.hpp"

#include <volk.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace tracey
{
    namespace vops
    {
        namespace codegen
        {
            namespace
            {
                // Workgroup size baked into the emitted shader header.
                // The emitter currently locks this at 64 (see
                // glsl_emit.cpp); we still propagate it from the emit
                // result so future tuning is one-touch.
                uint32_t dispatchGroups(size_t pointCount, uint32_t localSizeX)
                {
                    return static_cast<uint32_t>(
                        (pointCount + localSizeX - 1) / localSizeX);
                }

                [[noreturn]] void vkThrow(const char *where, VkResult r)
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                        "[vop:compute] %s failed: VkResult=%d", where,
                        static_cast<int>(r));
                    throw std::runtime_error(buf);
                }
                void vkCheck(VkResult r, const char *where)
                {
                    if (r != VK_SUCCESS) vkThrow(where, r);
                }

                // Look up an attribute on the geometry's point table.
                // Returns nullptr when missing. Templated so we can do
                // the same lookup for both float and Vec3 stores.
                // Note: Vec3 std430 packing (16-byte stride) is now
                // owned by Attribute<Vec3>::uploadCpuToGpu — Phase A.
                template <typename T>
                Attribute<T> *findPointAttr(Geometry &g, const std::string &name)
                {
                    return g.points().get<T>(name);
                }

                // Canonical default value for a named attribute, used when
                // the dispatcher needs to materialise it on a geometry
                // that didn't carry it. Houdini-style conventions:
                //   Cd  → white (1,1,1) so a geo_input.Cd → geo_output.Cd
                //         passthrough on geometry without Cd stays
                //         indistinguishable from "no Cd at all" — the
                //         rasterizer's no-color fallback also fills
                //         the colour buffer with white.
                //   N   → up (0,1,0) — surface normals default to
                //         world-up when missing.
                //   pscale → 1.0 — instance scale identity.
                //   Alpha  → 1.0 — fully opaque.
                // Everything else falls through to zero. The actual table
                // lives in geo_io_ports.hpp, shared with the CPU nodes
                // and the GLSL emitter.
                Vec3 defaultVec3For(const std::string &name)
                {
                    return geoDefaultVec3For(name);
                }
                float defaultFloatFor(const std::string &name)
                {
                    return geoDefaultFloatFor(name);
                }

                // Live-param read for one ParamSlot. The emitter assigned
                // slot indices in declaration order — for each slot we
                // pull the VopNode's matching parameter and pack into a
                // vec4 (float in .x, vec3 in .xyz, int as float).
                Vec4 packParamSlot(const VopGraph &graph, const ParamSlot &p)
                {
                    const VopNode *node = graph.findNode(p.nodeUid);
                    if (!node) return Vec4(0.0f);
                    switch (p.type)
                    {
                    case GpuType::Float:
                        return Vec4(node->paramFloat(p.paramName, 0.0f), 0, 0, 0);
                    case GpuType::Int:
                        return Vec4(
                            static_cast<float>(node->paramInt(p.paramName, 0)),
                            0, 0, 0);
                    case GpuType::Vec3:
                    {
                        const Vec3 v = node->paramVec3(p.paramName, Vec3(0.0f));
                        return Vec4(v.x, v.y, v.z, 0.0f);
                    }
                    }
                    return Vec4(0.0f);
                }
            } // anon

            // Full type now that VulkanComputeDevice is included.
            struct VopComputeDispatcher::PipelineEntry
            {
                VkDevice device = VK_NULL_HANDLE;
                VkShaderModule shaderModule = VK_NULL_HANDLE;
                VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
                VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
                VkPipeline pipeline = VK_NULL_HANDLE;
                EmitResult emit;

                ~PipelineEntry()
                {
                    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
                    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
                    if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
                    if (shaderModule) vkDestroyShaderModule(device, shaderModule, nullptr);
                }
            };

            namespace
            {
                // Single process-wide dispatcher pointer + a coarse
                // mutex that serialises GPU work. The mutex lives
                // outside the dispatcher class so the cook paths can
                // hold it across the dispatch() call without each
                // caller having to know about Vulkan internals.
                std::atomic<VopComputeDispatcher *> g_dispatcher{nullptr};
                // No per-class mutex: dispatch() locks the process-wide
                // vulkanQueueMutex (which covers both command-pool and
                // queue access) for the entire critical section. The
                // previous private mutex only serialised same-class
                // calls — it didn't keep this dispatcher from racing
                // the render thread or another dispatcher's command
                // recording against the shared command pool.
            }

            VopComputeDispatcher *VopComputeDispatcher::getGlobal()
            {
                return g_dispatcher.load(std::memory_order_acquire);
            }
            void VopComputeDispatcher::setGlobal(VopComputeDispatcher *d)
            {
                g_dispatcher.store(d, std::memory_order_release);
            }

            VopComputeDispatcher::VopComputeDispatcher(Device *device)
                : m_device(dynamic_cast<VulkanComputeDevice *>(device))
            {
                if (!device) throw std::runtime_error(
                    "[vop:compute] device is null");
                if (!m_device) throw std::runtime_error(
                    "[vop:compute] device is not a VulkanComputeDevice "
                    "(GPU compute backend required)");
            }
            VopComputeDispatcher::~VopComputeDispatcher() = default;

            VopComputeDispatcher::PipelineEntry &
            VopComputeDispatcher::compileOrGet(const VopGraph &graph)
            {
                EmitResult emit = emitGlsl(graph);
                if (!emit.unsupported.empty())
                {
                    std::string msg = "[vop:compute] graph has unsupported node(s):";
                    for (const auto &k : emit.unsupported) { msg += ' '; msg += k; }
                    throw std::runtime_error(msg);
                }
                const uint64_t key = hashGlsl(emit.glsl);
                auto it = m_cache.find(key);
                if (it != m_cache.end())
                {
                    // Overwrite with this cook's emit so the dispatcher
                    // sees the live param-slot layout (param VALUES
                    // change every cook; positions/layouts are stable
                    // for a given hash but we keep the data fresh).
                    it->second->emit = std::move(emit);
                    return *it->second;
                }

                // Compile.
                ShaderCompiler compiler;
                std::vector<uint32_t> spirv;
                try
                {
                    spirv = compiler.compileComputeShader(emit.glsl, "vop_kernel");
                }
                catch (const std::exception &e)
                {
                    throw std::runtime_error(std::string(
                        "[vop:compute] GLSL→SPIRV failed: ") + e.what());
                }

                auto entry = std::make_unique<PipelineEntry>();
                entry->device = m_device->vkDevice();
                entry->emit = std::move(emit);

                // Shader module.
                VkShaderModuleCreateInfo smInfo{};
                smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                smInfo.codeSize = spirv.size() * sizeof(uint32_t);
                smInfo.pCode = spirv.data();
                vkCheck(vkCreateShaderModule(entry->device, &smInfo, nullptr,
                                              &entry->shaderModule),
                        "vkCreateShaderModule");

                // Descriptor set layout: one storage buffer per attr +
                // one for the param SSBO. Bindings 0..N-1 = attrs,
                // binding N = params (matches emitter's binding indexing).
                std::vector<VkDescriptorSetLayoutBinding> bindings;
                bindings.reserve(entry->emit.attrs.size() + 1);
                for (const auto &a : entry->emit.attrs)
                {
                    VkDescriptorSetLayoutBinding b{};
                    b.binding = a.binding;
                    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    b.descriptorCount = 1;
                    b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                    bindings.push_back(b);
                }
                {
                    VkDescriptorSetLayoutBinding b{};
                    b.binding = entry->emit.paramsBinding;
                    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    b.descriptorCount = 1;
                    b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                    bindings.push_back(b);
                }
                VkDescriptorSetLayoutCreateInfo dslInfo{};
                dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
                dslInfo.pBindings = bindings.data();
                vkCheck(vkCreateDescriptorSetLayout(entry->device, &dslInfo, nullptr,
                                                     &entry->setLayout),
                        "vkCreateDescriptorSetLayout");

                // Pipeline layout: descriptor set + uint pointCount push constant.
                VkPushConstantRange pcRange{};
                pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                pcRange.offset = 0;
                pcRange.size = sizeof(uint32_t);

                VkPipelineLayoutCreateInfo plInfo{};
                plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                plInfo.setLayoutCount = 1;
                plInfo.pSetLayouts = &entry->setLayout;
                plInfo.pushConstantRangeCount = 1;
                plInfo.pPushConstantRanges = &pcRange;
                vkCheck(vkCreatePipelineLayout(entry->device, &plInfo, nullptr,
                                                &entry->pipelineLayout),
                        "vkCreatePipelineLayout");

                // Compute pipeline.
                VkComputePipelineCreateInfo cpInfo{};
                cpInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                cpInfo.stage.module = entry->shaderModule;
                cpInfo.stage.pName = "main";
                cpInfo.layout = entry->pipelineLayout;
                vkCheck(vkCreateComputePipelines(entry->device, VK_NULL_HANDLE, 1,
                                                  &cpInfo, nullptr, &entry->pipeline),
                        "vkCreateComputePipelines");

                auto *raw = entry.get();
                m_cache.emplace(key, std::move(entry));
                return *raw;
            }

            DispatchStats VopComputeDispatcher::dispatch(const VopGraph &graph,
                                                          Geometry &geo)
            {
                // Vulkan command-pool / descriptor-pool / queue access
                // is single-thread. The cook worker thread and the
                // synchronous export thread can both call into here;
                // serialise the whole dispatch end-to-end so neither
                // steps on the other.
                // Whole-dispatch lock: covers command-buffer alloc,
                // recording, submit, fence wait, and free.
                std::lock_guard<std::mutex> lock(vulkanQueueMutex());

                DispatchStats stats;
                stats.pointCount = geo.pointCount();
                if (stats.pointCount == 0) return stats;

                const bool wasCached = (m_cache.find(
                    hashGlsl(emitGlsl(graph).glsl)) != m_cache.end());
                stats.pipelineCached = wasCached;

                PipelineEntry &entry = compileOrGet(graph);
                const auto &emit = entry.emit;
                const VkDevice dev = entry.device;

                // ── Resolve attribute buffers (persistent) ────────────
                // Phase B: each Attribute<T> owns its own GPU SSBO and
                // handles upload/download itself. The dispatcher just
                // asks for it. bufferConst() for read-only inputs (no
                // generation bump, side stays at Both); buffer() for
                // anything we write (flips side to GPU, bumps gen, so
                // the next CPU read lazily downloads). Result: zero
                // upload + zero readback on the dispatcher's critical
                // path when buffers are already current from a prior
                // cook.
                std::vector<const Buffer *> attrBufs;
                attrBufs.reserve(emit.attrs.size());

                const auto tUpload = std::chrono::steady_clock::now();
                for (const auto &a : emit.attrs)
                {
                    AttributeBase *attr = nullptr;
                    if (a.type == GpuType::Vec3)
                    {
                        auto *typed = findPointAttr<Vec3>(geo, a.name);
                        if (!typed)
                        {
                            // Missing attribute: materialise it with the
                            // canonical default for that name (white for
                            // Cd, up for N, zero otherwise). Picking the
                            // right default matters for write-back
                            // semantics — a geo_input.Cd → geo_output.Cd
                            // passthrough on geometry that doesn't
                            // carry Cd would otherwise stamp the
                            // attribute with the read default (zeros),
                            // and the rasterizer then renders the
                            // surface black instead of leaving it at
                            // the implicit "no colours" white the
                            // scene compiler fills. defaultVec3For
                            // keeps the GPU path indistinguishable
                            // from "no attribute at all" downstream.
                            typed = geo.points().add<Vec3>(a.name, defaultVec3For(a.name));
                            if (typed->size() < stats.pointCount)
                                typed->resize(stats.pointCount);
                        }
                        attr = typed;
                    }
                    else  // Float
                    {
                        auto *typed = findPointAttr<float>(geo, a.name);
                        if (!typed)
                        {
                            // Same materialisation policy as the Vec3
                            // branch above. pscale / Alpha get 1.0;
                            // everything else gets 0.
                            typed = geo.points().add<float>(a.name, defaultFloatFor(a.name));
                            if (typed->size() < stats.pointCount)
                                typed->resize(stats.pointCount);
                        }
                        attr = typed;
                    }

                    const Buffer *buf = a.write ? attr->buffer() : attr->bufferConst();
                    if (!buf) throw std::runtime_error(
                        "[vop:compute] GPU storage unavailable for attribute: " + a.name);
                    attrBufs.push_back(buf);
                }

                // ── Params SSBO (still transient) ─────────────────────
                // Param values change every cook (slider drags, animated
                // promoted params, the playhead's frame stamp). The
                // pipeline cache hits on graph structure, so we just
                // upload the latest values each call. Cheap — typically
                // <100 vec4 slots.
                const size_t paramBytes = emit.paramSlotCount * sizeof(Vec4);
                std::unique_ptr<Buffer> paramBuf(m_device->createBuffer(
                    static_cast<uint32_t>(paramBytes),
                    BufferUsage::StorageBuffer));
                {
                    void *dst = paramBuf->mapForWriting();
                    auto *slots = static_cast<Vec4 *>(dst);
                    for (size_t i = 0; i < emit.paramSlotCount; ++i)
                        slots[i] = Vec4(0.0f);
                    for (const auto &p : emit.params)
                        slots[p.slot] = packParamSlot(graph, p);
                    paramBuf->unmap();
                }
                stats.uploadMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tUpload).count();

                // ── Descriptor set ────────────────────────────────────
                VkDescriptorSetAllocateInfo dsAlloc{};
                dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                dsAlloc.descriptorPool = m_device->descriptorPool();
                dsAlloc.descriptorSetCount = 1;
                dsAlloc.pSetLayouts = &entry.setLayout;
                VkDescriptorSet descSet = VK_NULL_HANDLE;
                vkCheck(vkAllocateDescriptorSets(dev, &dsAlloc, &descSet),
                        "vkAllocateDescriptorSets");

                std::vector<VkDescriptorBufferInfo> bufInfos;
                bufInfos.reserve(emit.attrs.size() + 1);
                std::vector<VkWriteDescriptorSet> writes;
                writes.reserve(emit.attrs.size() + 1);

                for (size_t i = 0; i < emit.attrs.size(); ++i)
                {
                    const size_t bytes = stats.pointCount *
                        ((emit.attrs[i].type == GpuType::Vec3) ? 16u : 4u);
                    VkDescriptorBufferInfo bi{};
                    // The attribute's Buffer is owned by the Geometry;
                    // we only need its VkBuffer handle for binding.
                    // VulkanBuffer::vkBuffer() is const-callable; the
                    // descriptor write is read-only w.r.t. the handle.
                    bi.buffer = static_cast<const VulkanBuffer *>(attrBufs[i])->vkBuffer();
                    bi.offset = 0;
                    bi.range = bytes;
                    bufInfos.push_back(bi);
                    VkWriteDescriptorSet w{};
                    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstSet = descSet;
                    w.dstBinding = emit.attrs[i].binding;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w.pBufferInfo = &bufInfos.back();
                    writes.push_back(w);
                }
                {
                    VkDescriptorBufferInfo bi{};
                    bi.buffer = static_cast<VulkanBuffer *>(paramBuf.get())->vkBuffer();
                    bi.offset = 0;
                    bi.range  = paramBytes;
                    bufInfos.push_back(bi);
                    VkWriteDescriptorSet w{};
                    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstSet = descSet;
                    w.dstBinding = emit.paramsBinding;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w.pBufferInfo = &bufInfos.back();
                    writes.push_back(w);
                }
                vkUpdateDescriptorSets(dev,
                    static_cast<uint32_t>(writes.size()), writes.data(),
                    0, nullptr);

                // ── Command buffer + dispatch ─────────────────────────
                VkCommandBufferAllocateInfo cmdAlloc{};
                cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cmdAlloc.commandPool = m_device->commandPool();
                cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cmdAlloc.commandBufferCount = 1;
                VkCommandBuffer cmd = VK_NULL_HANDLE;
                vkCheck(vkAllocateCommandBuffers(dev, &cmdAlloc, &cmd),
                        "vkAllocateCommandBuffers");

                VkCommandBufferBeginInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkCheck(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        entry.pipelineLayout, 0, 1, &descSet, 0, nullptr);
                const uint32_t pointCount = static_cast<uint32_t>(stats.pointCount);
                vkCmdPushConstants(cmd, entry.pipelineLayout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(uint32_t), &pointCount);
                vkCmdDispatch(cmd, dispatchGroups(stats.pointCount, emit.localSizeX),
                              1, 1);

                // Make the compute writes visible to any future host
                // map (the next call to attribute.dataConst() that
                // downloads). HOST_COHERENT memory still needs this
                // shader-write → host-read barrier on the GPU side.
                VkMemoryBarrier mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_HOST_BIT,
                    0,
                    1, &mb,
                    0, nullptr,
                    0, nullptr);
                vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

                VkFenceCreateInfo fci{};
                fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                VkFence fence = VK_NULL_HANDLE;
                vkCheck(vkCreateFence(dev, &fci, nullptr, &fence),
                        "vkCreateFence");

                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmd;

                const auto tGpu = std::chrono::steady_clock::now();
                vkCheck(vkQueueSubmit(m_device->computeQueue(), 1, &si, fence),
                        "vkQueueSubmit");
                vkCheck(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX),
                        "vkWaitForFences");
                stats.gpuMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tGpu).count();

                vkDestroyFence(dev, fence, nullptr);
                vkFreeCommandBuffers(dev, m_device->commandPool(), 1, &cmd);
                vkFreeDescriptorSets(dev, m_device->descriptorPool(), 1, &descSet);

                // No explicit readback — write attributes were already
                // marked GPU-current via buffer() above, so the next
                // CPU read on those attributes will lazily download
                // through Attribute<T>::dataConst().
                stats.readbackMs = 0.0;
                return stats;
            }
        }
    }
}
