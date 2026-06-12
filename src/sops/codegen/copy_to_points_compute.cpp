// GPU dispatcher for copy_to_points. See header for the supported scope.
//
// Implementation follows the same pattern as VopComputeDispatcher in
// src/vops/codegen/compute_dispatch.cpp — one cached compute pipeline per
// distinct GLSL source, descriptor set per dispatch, one-shot command
// buffer, fence wait. Differences from the VOP path:
//   • the kernel is fixed (not user-graph-driven); we templatise via
//     #define-injected feature flags and key the cache by the feature
//     mask, so at most 32 variants exist process-wide
//   • the dispatch grid is N·M points, not pointCount — same width but
//     a different conceptual mapping (i = thread / M = template index,
//     j = thread % M = stamp vertex index)
//   • the output Geometry is owned by the caller; we resize it in-place
//     and stamp through Attribute<T>::buffer() so the result stays on
//     the GPU (subsequent rasterizer access reads directly off the
//     SSBO, the next CPU touch lazily downloads via syncToCpu())

#include "copy_to_points_compute.hpp"

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

#include <volk.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace codegen
        {
            namespace
            {
                // Feature mask bits. Stay in sync with the #defines emitted
                // in buildGlsl() below; the cache key is the bitwise OR.
                constexpr uint32_t kFeatStampN  = 1u << 0;
                constexpr uint32_t kFeatStampUV = 1u << 1;
                constexpr uint32_t kFeatTplN    = 1u << 2;
                constexpr uint32_t kFeatTplPs   = 1u << 3;
                constexpr uint32_t kFeatTplCd   = 1u << 4;

                constexpr uint32_t kLocalSizeX = 64;

                // Build the GLSL source with #defines selecting which of the
                // optional inputs/outputs are bound for this dispatch. The
                // binding indices are stable across variants so we can keep
                // the descriptor-set-layout build code simple; bindings
                // skipped by the active variant simply don't appear in the
                // shader (and we don't add them to the descriptor set).
                std::string buildGlsl(uint32_t features)
                {
                    std::string s;
                    s.reserve(2048);
                    s += "#version 450\n";
                    if (features & kFeatStampN)  s += "#define HAS_STAMP_N\n";
                    if (features & kFeatStampUV) s += "#define HAS_STAMP_UV\n";
                    if (features & kFeatTplN)    s += "#define HAS_TPL_N\n";
                    if (features & kFeatTplPs)   s += "#define HAS_TPL_PS\n";
                    if (features & kFeatTplCd)   s += "#define HAS_TPL_CD\n";
                    s += "layout(local_size_x = 64) in;\n";

                    // Binding map (compact — only present-features bind):
                    //   0  stampP
                    //   1  stampN     (if HAS_STAMP_N)
                    //   2  stampUV    (if HAS_STAMP_UV)
                    //   3  tplP
                    //   4  tplN       (if HAS_TPL_N)
                    //   5  tplPs      (if HAS_TPL_PS)
                    //   6  tplCd      (if HAS_TPL_CD)
                    //   7  outP
                    //   8  outN       (if HAS_STAMP_N)
                    //   9  outUV      (if HAS_STAMP_UV)
                    //  10  outCd      (if HAS_TPL_CD)
                    s += "layout(std430, binding = 0) readonly buffer InStampP { vec4 data[]; } stampP;\n";
                    if (features & kFeatStampN)
                        s += "layout(std430, binding = 1) readonly buffer InStampN { vec4 data[]; } stampN;\n";
                    if (features & kFeatStampUV)
                        s += "layout(std430, binding = 2) readonly buffer InStampUV { vec2 data[]; } stampUV;\n";
                    s += "layout(std430, binding = 3) readonly buffer InTplP { vec4 data[]; } tplP;\n";
                    if (features & kFeatTplN)
                        s += "layout(std430, binding = 4) readonly buffer InTplN { vec4 data[]; } tplN;\n";
                    if (features & kFeatTplPs)
                        s += "layout(std430, binding = 5) readonly buffer InTplPs { float data[]; } tplPs;\n";
                    if (features & kFeatTplCd)
                        s += "layout(std430, binding = 6) readonly buffer InTplCd { vec4 data[]; } tplCd;\n";
                    s += "layout(std430, binding = 7) writeonly buffer OutP { vec4 data[]; } outP;\n";
                    if (features & kFeatStampN)
                        s += "layout(std430, binding = 8) writeonly buffer OutN { vec4 data[]; } outN;\n";
                    if (features & kFeatStampUV)
                        s += "layout(std430, binding = 9) writeonly buffer OutUV { vec2 data[]; } outUV;\n";
                    if (features & kFeatTplCd)
                        s += "layout(std430, binding = 10) writeonly buffer OutCd { vec4 data[]; } outCd;\n";

                    s += R"(
layout(push_constant) uniform PC {
    uint M;
    uint N;
    uint orient_to_normal;
    uint _pad;
} pc;

// Build a rotation matrix mapping the stamp's local +Z to `n` with up = +Y.
// Matches CPU orientFromNormal in copy_to_points_sop.cpp: identity when
// `n` is near-zero or parallel to up.
mat3 orient_from_normal(vec3 n) {
    float len2 = dot(n, n);
    if (len2 < 1e-12) return mat3(1.0);
    vec3 forward = n * inversesqrt(len2);
    vec3 upRef = vec3(0.0, 1.0, 0.0);
    vec3 right = cross(upRef, forward);
    float r2 = dot(right, right);
    if (r2 < 1e-12) return mat3(1.0);
    right *= inversesqrt(r2);
    vec3 newUp = cross(forward, right);
    return mat3(right, newUp, forward);
}

void main() {
    uint gid = gl_GlobalInvocationID.x;
    uint total = pc.N * pc.M;
    if (gid >= total) return;
    uint i = gid / pc.M;  // template point index
    uint j = gid % pc.M;  // stamp vertex index

    vec3 sp = stampP.data[j].xyz;
    vec3 tp = tplP.data[i].xyz;

#ifdef HAS_TPL_PS
    float s = tplPs.data[i];
#else
    float s = 1.0;
#endif

    mat3 R = mat3(1.0);
#ifdef HAS_TPL_N
    if (pc.orient_to_normal != 0u) {
        R = orient_from_normal(tplN.data[i].xyz);
    }
#endif

    vec3 worldP = R * (sp * s) + tp;
    outP.data[gid] = vec4(worldP, 0.0);

#ifdef HAS_STAMP_N
    vec3 sn = stampN.data[j].xyz;
    vec3 rn = R * sn;
    float l = length(rn);
    outN.data[gid] = vec4((l > 0.0) ? rn / l : sn, 0.0);
#endif

#ifdef HAS_STAMP_UV
    outUV.data[gid] = stampUV.data[j];
#endif

#ifdef HAS_TPL_CD
    outCd.data[gid] = vec4(tplCd.data[i].xyz, 0.0);
#endif
}
)";
                    return s;
                }

                [[noreturn]] void vkThrow(const char *where, VkResult r)
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                        "[ctp:compute] %s failed: VkResult=%d", where,
                        static_cast<int>(r));
                    throw std::runtime_error(buf);
                }
                void vkCheck(VkResult r, const char *where)
                {
                    if (r != VK_SUCCESS) vkThrow(where, r);
                }

                // Has the geometry's attribute table got *exactly* this
                // whitelist (allowing each entry to be present or absent)?
                // Returns false if any unexpected attribute exists.
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

                std::atomic<CopyToPointsCompute *> g_dispatcher{nullptr};
                // No per-class mutex; dispatch() acquires the global
                // vulkanQueueMutex covering the whole command-pool +
                // queue critical section.
            }

            CopyToPointsCompute *CopyToPointsCompute::getGlobal()
            {
                return g_dispatcher.load(std::memory_order_acquire);
            }
            void CopyToPointsCompute::setGlobal(CopyToPointsCompute *d)
            {
                g_dispatcher.store(d, std::memory_order_release);
            }

            // Per-feature-mask compiled pipeline. Cache lookup keyed by
            // features; one entry stays alive for the dispatcher's life.
            struct PipelineEntry
            {
                VkDevice device = VK_NULL_HANDLE;
                VkShaderModule shaderModule = VK_NULL_HANDLE;
                VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
                VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
                VkPipeline pipeline = VK_NULL_HANDLE;
                // Active bindings for this variant, in the order the
                // dispatcher will fill them. Encoded as `(binding, write)`
                // so descriptor writes can iterate without re-deriving.
                std::vector<uint32_t> bindings;

                ~PipelineEntry()
                {
                    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
                    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
                    if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
                    if (shaderModule) vkDestroyShaderModule(device, shaderModule, nullptr);
                }
            };

            struct CopyToPointsCompute::Impl
            {
                VulkanComputeDevice *device = nullptr;
                std::unordered_map<uint32_t, std::unique_ptr<PipelineEntry>> cache;

                PipelineEntry &compileOrGet(uint32_t features)
                {
                    auto it = cache.find(features);
                    if (it != cache.end()) return *it->second;

                    const std::string glsl = buildGlsl(features);
                    ShaderCompiler compiler;
                    std::vector<uint32_t> spirv;
                    try
                    {
                        spirv = compiler.compileComputeShader(glsl, "copy_to_points");
                    }
                    catch (const std::exception &e)
                    {
                        throw std::runtime_error(std::string(
                            "[ctp:compute] GLSL→SPIRV failed: ") + e.what());
                    }

                    auto entry = std::make_unique<PipelineEntry>();
                    entry->device = device->vkDevice();

                    VkShaderModuleCreateInfo smInfo{};
                    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                    smInfo.codeSize = spirv.size() * sizeof(uint32_t);
                    smInfo.pCode = spirv.data();
                    vkCheck(vkCreateShaderModule(entry->device, &smInfo, nullptr,
                                                  &entry->shaderModule),
                            "vkCreateShaderModule");

                    // Build the descriptor set layout — one binding per
                    // active input/output in this variant.
                    std::vector<VkDescriptorSetLayoutBinding> bindings;
                    auto add = [&](uint32_t b) {
                        VkDescriptorSetLayoutBinding x{};
                        x.binding = b;
                        x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        x.descriptorCount = 1;
                        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                        bindings.push_back(x);
                        entry->bindings.push_back(b);
                    };
                    add(0);  // stampP
                    if (features & kFeatStampN)  add(1);
                    if (features & kFeatStampUV) add(2);
                    add(3);  // tplP
                    if (features & kFeatTplN)    add(4);
                    if (features & kFeatTplPs)   add(5);
                    if (features & kFeatTplCd)   add(6);
                    add(7);  // outP
                    if (features & kFeatStampN)  add(8);
                    if (features & kFeatStampUV) add(9);
                    if (features & kFeatTplCd)   add(10);

                    VkDescriptorSetLayoutCreateInfo dslInfo{};
                    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                    dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
                    dslInfo.pBindings = bindings.data();
                    vkCheck(vkCreateDescriptorSetLayout(entry->device, &dslInfo, nullptr,
                                                         &entry->setLayout),
                            "vkCreateDescriptorSetLayout");

                    // 16-byte push constant: M, N, orient, pad.
                    VkPushConstantRange pcRange{};
                    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                    pcRange.offset = 0;
                    pcRange.size = 4 * sizeof(uint32_t);

                    VkPipelineLayoutCreateInfo plInfo{};
                    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                    plInfo.setLayoutCount = 1;
                    plInfo.pSetLayouts = &entry->setLayout;
                    plInfo.pushConstantRangeCount = 1;
                    plInfo.pPushConstantRanges = &pcRange;
                    vkCheck(vkCreatePipelineLayout(entry->device, &plInfo, nullptr,
                                                    &entry->pipelineLayout),
                            "vkCreatePipelineLayout");

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
                    cache.emplace(features, std::move(entry));
                    return *raw;
                }
            };

            CopyToPointsCompute::CopyToPointsCompute(Device *device)
                : m_impl(new Impl)
            {
                m_impl->device = dynamic_cast<VulkanComputeDevice *>(device);
                if (!device) throw std::runtime_error("[ctp:compute] device is null");
                if (!m_impl->device) throw std::runtime_error(
                    "[ctp:compute] device is not a VulkanComputeDevice");
            }
            CopyToPointsCompute::~CopyToPointsCompute()
            {
                delete m_impl;
            }

            bool CopyToPointsCompute::dispatch(const Geometry &stamp,
                                                const Geometry &tmpl,
                                                bool orient_to_normal,
                                                Geometry &out) noexcept
            {
                try
                {
                    const auto &stampPts  = stamp.points();
                    const auto &stampVrts = stamp.vertices();
                    const auto &stampPrms = stamp.primitives();
                    const auto &tplPts    = tmpl.points();

                    // Scope checks. Anything outside the v1 envelope returns
                    // false so the caller falls back to the CPU evaluator.
                    if (!attrsAreSubsetOf(stampPts,  {"P", "N"}))     return false;
                    if (!attrsAreSubsetOf(stampVrts, {"uv"}))         return false;
                    if (stampPrms.names().size() != 0)                return false;

                    // Per-clone `orient` quaternions (effector output) have
                    // no kernel path yet. The template-attr fetches below are
                    // ad-hoc (unknown attrs are otherwise silently ignored),
                    // so without this guard orient-carrying templates would
                    // dispatch fine and produce UNROTATED clones.
                    if (tplPts.get<Vec4>("orient"))                   return false;

                    // The kernel writes outP[i*M + j] = transform(stamp[j]).
                    // That implicitly assumes vertexToPoint is identity — i.e.
                    // each vertex c references point c. The fromSceneObject
                    // pipeline always produces per-corner-unique geometry so
                    // this holds for the common case; complex stamps that
                    // weld points would break it.
                    if (stamp.pointCount() != stamp.vertexCount())    return false;
                    const auto &vtp = stamp.vertexToPoint();
                    for (size_t i = 0; i < vtp.size(); ++i)
                    {
                        if (vtp[i] != static_cast<uint32_t>(i)) return false;
                    }

                    const auto *stampP_attr  = stampPts.get<Vec3>("P");
                    if (!stampP_attr)                                 return false;
                    const auto *stampN_attr  = stampPts.get<Vec3>("N");
                    const auto *stampUV_attr = stampVrts.get<Vec2>("uv");
                    const auto *tplP_attr    = tplPts.get<Vec3>("P");
                    if (!tplP_attr)                                   return false;
                    const auto *tplN_attr    = tplPts.get<Vec3>("N");
                    const auto *tplPs_attr   = tplPts.get<float>("pscale");
                    const auto *tplCd_attr   = tplPts.get<Vec3>("Cd");

                    const size_t M = stamp.pointCount();
                    const size_t N = tmpl.pointCount();
                    if (M == 0 || N == 0) return false;
                    const size_t totalP = N * M;
                    const size_t totalV = N * stamp.vertexCount();
                    const size_t totalT = N * stampPrms.size();

                    uint32_t features = 0;
                    if (stampN_attr)  features |= kFeatStampN;
                    if (stampUV_attr) features |= kFeatStampUV;
                    if (tplN_attr)    features |= kFeatTplN;
                    if (tplPs_attr)   features |= kFeatTplPs;
                    if (tplCd_attr)   features |= kFeatTplCd;

                    // Whole-dispatch lock — see vulkan_queue_sync.hpp.
                    std::lock_guard<std::mutex> lock(vulkanQueueMutex());
                    PipelineEntry &entry = m_impl->compileOrGet(features);
                    const VkDevice dev = entry.device;

                    // ── Resolve input buffers (read-only) ───────────
                    auto inBuf = [](const AttributeBase *a) -> const Buffer * {
                        return a ? a->bufferConst() : nullptr;
                    };
                    const Buffer *bStampP = inBuf(stampP_attr);
                    const Buffer *bStampN = stampN_attr  ? inBuf(stampN_attr)  : nullptr;
                    const Buffer *bStampUV = stampUV_attr ? inBuf(stampUV_attr) : nullptr;
                    const Buffer *bTplP   = inBuf(tplP_attr);
                    const Buffer *bTplN   = tplN_attr    ? inBuf(tplN_attr)    : nullptr;
                    const Buffer *bTplPs  = tplPs_attr   ? inBuf(tplPs_attr)   : nullptr;
                    const Buffer *bTplCd  = tplCd_attr   ? inBuf(tplCd_attr)   : nullptr;
                    if (!bStampP || !bTplP) return false;
                    if ((features & kFeatStampN)  && !bStampN)  return false;
                    if ((features & kFeatStampUV) && !bStampUV) return false;
                    if ((features & kFeatTplN)    && !bTplN)    return false;
                    if ((features & kFeatTplPs)   && !bTplPs)   return false;
                    if ((features & kFeatTplCd)   && !bTplCd)   return false;

                    // ── Prepare output Geometry ─────────────────────
                    // Build topology CPU-side (it's O(N · (VC + PC)) integer
                    // ops — orders of magnitude cheaper than the vertex
                    // transform we're punting to the GPU).
                    Geometry result;
                    if (features & kFeatStampN)
                        result.points().add<Vec3>("N", Vec3(0.0f, 1.0f, 0.0f));
                    if (features & kFeatStampUV)
                        result.vertices().add<Vec2>("uv", Vec2(0.0f));
                    if (features & kFeatTplCd)
                        result.vertices().add<Vec3>("Cd", Vec3(1.0f));

                    result.points().resize(totalP);
                    result.vertices().resize(totalV);

                    auto &outVtp = result.vertexToPoint();
                    outVtp.reserve(totalV);
                    for (size_t i = 0; i < N; ++i)
                    {
                        const uint32_t base = static_cast<uint32_t>(i * M);
                        for (size_t v = 0; v < vtp.size(); ++v)
                            outVtp.push_back(base + vtp[v]);
                    }

                    auto &outPrims = result.primitivesList();
                    outPrims.reserve(totalT);
                    const auto &stampPrimList = stamp.primitivesList();
                    for (size_t i = 0; i < N; ++i)
                    {
                        const uint32_t voff = static_cast<uint32_t>(i * stamp.vertexCount());
                        for (const auto &p : stampPrimList)
                        {
                            outPrims.push_back({p.firstVertex + voff, p.vertexCount});
                        }
                    }
                    result.primitives().resize(totalT);

                    // Output attribute buffers. resize() sized them above;
                    // calling buffer() allocates the GPU SSBO + flips side
                    // to Gpu so the kernel can write directly. The
                    // dispatcher relies on these buffer handles staying
                    // alive for the lifetime of the submit (they do —
                    // result outlives this dispatch).
                    auto outBufFor = [](AttributeBase *a) -> Buffer * {
                        return a ? a->buffer() : nullptr;
                    };
                    Buffer *bOutP = outBufFor(result.points().get<Vec3>("P"));
                    Buffer *bOutN = (features & kFeatStampN)
                        ? outBufFor(result.points().get<Vec3>("N")) : nullptr;
                    Buffer *bOutUV = (features & kFeatStampUV)
                        ? outBufFor(result.vertices().get<Vec2>("uv")) : nullptr;
                    Buffer *bOutCd = (features & kFeatTplCd)
                        ? outBufFor(result.vertices().get<Vec3>("Cd")) : nullptr;
                    if (!bOutP) return false;
                    if ((features & kFeatStampN)  && !bOutN)  return false;
                    if ((features & kFeatStampUV) && !bOutUV) return false;
                    if ((features & kFeatTplCd)   && !bOutCd) return false;

                    // ── Descriptor set ──────────────────────────────
                    VkDescriptorSetAllocateInfo dsAlloc{};
                    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                    dsAlloc.descriptorPool = m_impl->device->descriptorPool();
                    dsAlloc.descriptorSetCount = 1;
                    dsAlloc.pSetLayouts = &entry.setLayout;
                    VkDescriptorSet descSet = VK_NULL_HANDLE;
                    vkCheck(vkAllocateDescriptorSets(dev, &dsAlloc, &descSet),
                            "vkAllocateDescriptorSets");

                    struct BindSpec { uint32_t binding; const Buffer *buf; size_t bytes; };
                    std::vector<BindSpec> specs;
                    specs.reserve(entry.bindings.size());

                    const size_t vec3Stride = 16;
                    const size_t vec2Stride = 8;
                    const size_t floatStride = 4;

                    auto pushSpec = [&](uint32_t binding, const Buffer *buf, size_t bytes) {
                        specs.push_back({binding, buf, bytes});
                    };
                    pushSpec(0, bStampP, M * vec3Stride);
                    if (features & kFeatStampN)  pushSpec(1, bStampN,  M * vec3Stride);
                    if (features & kFeatStampUV) pushSpec(2, bStampUV, M * vec2Stride);
                    pushSpec(3, bTplP, N * vec3Stride);
                    if (features & kFeatTplN)    pushSpec(4, bTplN,  N * vec3Stride);
                    if (features & kFeatTplPs)   pushSpec(5, bTplPs, N * floatStride);
                    if (features & kFeatTplCd)   pushSpec(6, bTplCd, N * vec3Stride);
                    pushSpec(7, bOutP, totalP * vec3Stride);
                    if (features & kFeatStampN)  pushSpec(8, bOutN,  totalP * vec3Stride);
                    if (features & kFeatStampUV) pushSpec(9, bOutUV, totalV * vec2Stride);
                    if (features & kFeatTplCd)   pushSpec(10, bOutCd, totalV * vec3Stride);

                    std::vector<VkDescriptorBufferInfo> bufInfos(specs.size());
                    std::vector<VkWriteDescriptorSet> writes(specs.size());
                    for (size_t i = 0; i < specs.size(); ++i)
                    {
                        bufInfos[i] = {};
                        bufInfos[i].buffer = static_cast<const VulkanBuffer *>(specs[i].buf)->vkBuffer();
                        bufInfos[i].offset = 0;
                        bufInfos[i].range  = specs[i].bytes;
                        writes[i] = {};
                        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[i].dstSet = descSet;
                        writes[i].dstBinding = specs[i].binding;
                        writes[i].descriptorCount = 1;
                        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        writes[i].pBufferInfo = &bufInfos[i];
                    }
                    vkUpdateDescriptorSets(dev,
                        static_cast<uint32_t>(writes.size()), writes.data(),
                        0, nullptr);

                    // ── Command buffer + dispatch ───────────────────
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

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            entry.pipelineLayout, 0, 1, &descSet, 0, nullptr);

                    uint32_t pc[4] = {
                        static_cast<uint32_t>(M),
                        static_cast<uint32_t>(N),
                        orient_to_normal ? 1u : 0u,
                        0u,
                    };
                    vkCmdPushConstants(cmd, entry.pipelineLayout,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(pc), pc);

                    const uint32_t groups = static_cast<uint32_t>(
                        (totalP + kLocalSizeX - 1) / kLocalSizeX);
                    vkCmdDispatch(cmd, groups, 1, 1);

                    VkMemoryBarrier mb{};
                    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0,
                        1, &mb,
                        0, nullptr,
                        0, nullptr);

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
                    vkFreeDescriptorSets(dev, m_impl->device->descriptorPool(), 1, &descSet);

                    out = std::move(result);
                    return true;
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr,
                        "[ctp:compute] GPU dispatch failed, CPU fallback: %s\n",
                        e.what());
                    return false;
                }
                catch (...)
                {
                    std::fprintf(stderr,
                        "[ctp:compute] GPU dispatch failed (unknown), CPU fallback\n");
                    return false;
                }
            }
        }
    }
}
