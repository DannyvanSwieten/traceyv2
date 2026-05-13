// In-place per-element SRT transform on an Attribute<Vec3> GPU buffer.
// See transform_compute.hpp for the contract / scope.
//
// Implementation mirrors CopyToPointsCompute: one cached compute
// pipeline, one descriptor set per dispatch, fence wait. The kernel is
// fixed; the mode (position vs normal) is selected by a push-constant
// flag so we don't have to compile two variants. Push-constant payload
// is the 4x4 transform + count + mode (96 bytes total — within the
// 128-byte limit even on conservative MoltenVK builds).

#include "transform_compute.hpp"

#include "../../device/buffer.hpp"
#include "../../device/device.hpp"
#include "../../device/gpu/vulkan_buffer.hpp"
#include "../../device/gpu/vulkan_compute_device.hpp"
#include "../../geometry/attribute.hpp"
#include "../../gpu/shader_compiler.hpp"
#include "../../gpu/vulkan_queue_sync.hpp"

#include <volk.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
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
                constexpr uint32_t kLocalSizeX = 64;

                // Mode encoding in the push constant matches enum order.
                constexpr uint32_t kModePosition = 0;
                constexpr uint32_t kModeNormal   = 1;

                const char *kShaderSrc = R"(#version 450
layout(local_size_x = 64) in;

layout(std430, binding = 0) buffer Data { vec4 data[]; } b;

layout(push_constant) uniform PC {
    // Column-major 4x4 transform. Position mode multiplies vec4(p, 1),
    // Normal mode multiplies vec4(n, 0) (the translation column is
    // ignored — caller passes a pure-rotation matrix for parity with
    // the CPU SOP's "approximate" normal handling).
    mat4 m;
    uint count;
    uint mode;      // 0 = position, 1 = normal
    uint _pad0;
    uint _pad1;
} pc;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    vec3 v = b.data[i].xyz;
    vec3 r;
    if (pc.mode == 1u) {
        r = (pc.m * vec4(v, 0.0)).xyz;
        float l = length(r);
        if (l > 0.0) r /= l;
    } else {
        r = (pc.m * vec4(v, 1.0)).xyz;
    }
    b.data[i] = vec4(r, 0.0);
}
)";

                [[noreturn]] void vkThrow(const char *where, VkResult r)
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                        "[xform:compute] %s failed: VkResult=%d", where,
                        static_cast<int>(r));
                    throw std::runtime_error(buf);
                }
                void vkCheck(VkResult r, const char *where)
                {
                    if (r != VK_SUCCESS) vkThrow(where, r);
                }

                std::atomic<TransformCompute *> g_dispatcher{nullptr};
                // Whole-dispatch lock lives in vulkanQueueMutex; no
                // per-class mutex needed.
            }

            TransformCompute *TransformCompute::getGlobal()
            {
                return g_dispatcher.load(std::memory_order_acquire);
            }
            void TransformCompute::setGlobal(TransformCompute *d)
            {
                g_dispatcher.store(d, std::memory_order_release);
            }

            struct TransformCompute::Impl
            {
                VulkanComputeDevice *device = nullptr;
                VkShaderModule shaderModule = VK_NULL_HANDLE;
                VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
                VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
                VkPipeline pipeline = VK_NULL_HANDLE;

                ~Impl()
                {
                    if (!device) return;
                    VkDevice dev = device->vkDevice();
                    if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
                    if (pipelineLayout) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
                    if (setLayout) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
                    if (shaderModule) vkDestroyShaderModule(dev, shaderModule, nullptr);
                }

                void build()
                {
                    ShaderCompiler compiler;
                    std::vector<uint32_t> spirv =
                        compiler.compileComputeShader(kShaderSrc, "transform_kernel");

                    VkDevice dev = device->vkDevice();

                    VkShaderModuleCreateInfo smInfo{};
                    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                    smInfo.codeSize = spirv.size() * sizeof(uint32_t);
                    smInfo.pCode = spirv.data();
                    vkCheck(vkCreateShaderModule(dev, &smInfo, nullptr, &shaderModule),
                            "vkCreateShaderModule");

                    VkDescriptorSetLayoutBinding b{};
                    b.binding = 0;
                    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    b.descriptorCount = 1;
                    b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                    VkDescriptorSetLayoutCreateInfo dslInfo{};
                    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                    dslInfo.bindingCount = 1;
                    dslInfo.pBindings = &b;
                    vkCheck(vkCreateDescriptorSetLayout(dev, &dslInfo, nullptr, &setLayout),
                            "vkCreateDescriptorSetLayout");

                    // 96 bytes: mat4 (64) + 4 uints (16) = 80; padded by struct alignment.
                    // We declare 96 to give std140 mat4 its full layout slack.
                    VkPushConstantRange pcRange{};
                    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                    pcRange.offset = 0;
                    pcRange.size = sizeof(float) * 16 + sizeof(uint32_t) * 4;

                    VkPipelineLayoutCreateInfo plInfo{};
                    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                    plInfo.setLayoutCount = 1;
                    plInfo.pSetLayouts = &setLayout;
                    plInfo.pushConstantRangeCount = 1;
                    plInfo.pPushConstantRanges = &pcRange;
                    vkCheck(vkCreatePipelineLayout(dev, &plInfo, nullptr, &pipelineLayout),
                            "vkCreatePipelineLayout");

                    VkComputePipelineCreateInfo cpInfo{};
                    cpInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                    cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    cpInfo.stage.module = shaderModule;
                    cpInfo.stage.pName = "main";
                    cpInfo.layout = pipelineLayout;
                    vkCheck(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1,
                                                      &cpInfo, nullptr, &pipeline),
                            "vkCreateComputePipelines");
                }
            };

            TransformCompute::TransformCompute(Device *device)
                : m_impl(new Impl)
            {
                m_impl->device = dynamic_cast<VulkanComputeDevice *>(device);
                if (!device) throw std::runtime_error("[xform:compute] device is null");
                if (!m_impl->device) throw std::runtime_error(
                    "[xform:compute] device is not a VulkanComputeDevice");
                m_impl->build();
            }
            TransformCompute::~TransformCompute()
            {
                delete m_impl;
            }

            bool TransformCompute::dispatch(Attribute<Vec3> &attr,
                                             const float m[16],
                                             Mode mode) noexcept
            {
                try
                {
                    const size_t n = attr.size();
                    if (n == 0) return true;

                    std::lock_guard<std::mutex> lock(vulkanQueueMutex());
                    Buffer *buf = attr.buffer();  // forces upload + flips side to GPU
                    if (!buf) return false;
                    VkDevice dev = m_impl->device->vkDevice();

                    // Descriptor set — single storage-buffer binding.
                    VkDescriptorSetAllocateInfo dsAlloc{};
                    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                    dsAlloc.descriptorPool = m_impl->device->descriptorPool();
                    dsAlloc.descriptorSetCount = 1;
                    dsAlloc.pSetLayouts = &m_impl->setLayout;
                    VkDescriptorSet descSet = VK_NULL_HANDLE;
                    vkCheck(vkAllocateDescriptorSets(dev, &dsAlloc, &descSet),
                            "vkAllocateDescriptorSets");

                    VkDescriptorBufferInfo bi{};
                    bi.buffer = static_cast<VulkanBuffer *>(buf)->vkBuffer();
                    bi.offset = 0;
                    bi.range  = n * 16;  // std430 vec3 array stride

                    VkWriteDescriptorSet w{};
                    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstSet = descSet;
                    w.dstBinding = 0;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w.pBufferInfo = &bi;
                    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

                    // Command buffer.
                    VkCommandBufferAllocateInfo cmdAlloc{};
                    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    cmdAlloc.commandPool = m_impl->device->commandPool();
                    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    cmdAlloc.commandBufferCount = 1;
                    VkCommandBuffer cmd = VK_NULL_HANDLE;
                    vkCheck(vkAllocateCommandBuffers(dev, &cmdAlloc, &cmd),
                            "vkAllocateCommandBuffers");

                    VkCommandBufferBeginInfo beginInfo{};
                    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkCheck(vkBeginCommandBuffer(cmd, &beginInfo), "vkBeginCommandBuffer");

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            m_impl->pipelineLayout, 0, 1, &descSet, 0, nullptr);

                    // Push constants: matrix + count + mode.
                    struct PC {
                        float matrix[16];
                        uint32_t count;
                        uint32_t mode;
                        uint32_t pad0;
                        uint32_t pad1;
                    } pc{};
                    std::memcpy(pc.matrix, m, sizeof(pc.matrix));
                    pc.count = static_cast<uint32_t>(n);
                    pc.mode  = (mode == Mode::Normal) ? kModeNormal : kModePosition;
                    vkCmdPushConstants(cmd, m_impl->pipelineLayout,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(pc), &pc);

                    const uint32_t groups = static_cast<uint32_t>(
                        (n + kLocalSizeX - 1) / kLocalSizeX);
                    vkCmdDispatch(cmd, groups, 1, 1);

                    VkMemoryBarrier mb{};
                    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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
                    vkFreeDescriptorSets(dev, m_impl->device->descriptorPool(), 1, &descSet);
                    return true;
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr,
                        "[xform:compute] GPU dispatch failed, CPU fallback: %s\n",
                        e.what());
                    return false;
                }
                catch (...) { return false; }
            }
        }
    }
}
