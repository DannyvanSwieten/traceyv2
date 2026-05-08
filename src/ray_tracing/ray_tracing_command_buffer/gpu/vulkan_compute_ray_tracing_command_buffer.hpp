#pragma once
#include <volk.h>
#include <vector>
#include "../ray_tracing_command_buffer.hpp"

namespace tracey
{
    class VulkanComputeDevice;
    class VulkanWaveFrontPipeline;

    class VulkanComputeRayTracingCommandBuffer : public RayTracingCommandBuffer
    {
    public:
        VulkanComputeRayTracingCommandBuffer(VulkanComputeDevice &device);
        ~VulkanComputeRayTracingCommandBuffer() override;

        VkCommandBuffer vkCommandBuffer() const { return m_vkCommandBuffer; }

        void begin() override;
        void end() override;
        void setPipeline(RayTracingPipeline *pipeline) override;
        void setDescriptorSet(DescriptorSet *set) override;
        void traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height,
                       const TraceRaysParams &params = TraceRaysParams{}) override;
        void copyImageToBuffer(const Image2D *image, Buffer *buffer) override;
        void clearImage(Image2D *image, float r, float g, float b, float a) override;

        void waitUntilCompleted() override;

    private:
        // ============================================================================
        // Wavefront Pipeline Execution Helpers
        // ============================================================================

        /// Push constants passed to all wavefront shader stages.
        ///
        /// `sampleIndex` is the 0-based sample number within the current
        /// `traceRays` batch (i.e. ranges 0..samplesPerFrame-1). The resolve
        /// shader uses it together with `currentSample` (a uniform that holds
        /// the 1-based render-call count) to compute the global sample number
        /// for proper running-average accumulation across frames.
        struct WavefrontPushConstants
        {
            uint32_t width;
            uint32_t height;
            uint32_t sampleIndex;
            uint32_t samplesPerFrame;
        };

        /// Initializes payload and hit info buffers to their default values.
        /// Payloads are zeroed, hit info is set to 0xFFFFFFFF (invalid triangle).
        void initializeWavefrontBuffers(VulkanWaveFrontPipeline *wavefront);

        /// Clears hit info buffer to invalid triangle indices (0xFFFFFFFF).
        /// Called at the start of each sample to reset intersection state.
        void clearHitInfoBuffer(VulkanWaveFrontPipeline *wavefront);

        /// Clears the primary ray queue counter before ray generation.
        /// Must be called before dispatchRayGeneration since ray gen uses atomicAdd.
        void clearRayQueueForRayGen(VulkanWaveFrontPipeline *wavefront);

        /// Executes the ray generation shader to spawn primary rays.
        /// Populates the path header buffer and ray queue with initial rays.
        void dispatchRayGeneration(VulkanWaveFrontPipeline *wavefront,
                                   const WavefrontPushConstants &pushConstants,
                                   uint32_t workGroups);

        /// Clears queue counters and hit info buffer at the start of each bounce.
        /// Resets: next ray queue, hit queue, miss queue, and hit info buffer.
        void clearBounceBuffers(VulkanWaveFrontPipeline *wavefront, uint32_t bounce);

        /// Computes indirect dispatch arguments from queue counts.
        /// Reads ray/hit/miss queue counts and writes workgroup counts to indirect buffers.
        void dispatchPrepareIndirect(VulkanWaveFrontPipeline *wavefront);

        /// Executes the intersection shader to test rays against the scene.
        /// Populates hit info buffer and sorts rays into hit/miss queues.
        void dispatchIntersection(VulkanWaveFrontPipeline *wavefront,
                                  const WavefrontPushConstants &pushConstants,
                                  uint32_t workGroups);

        /// Executes the closest hit shader for rays that hit geometry.
        /// Processes material shading and may spawn secondary rays.
        void dispatchHitShader(VulkanWaveFrontPipeline *wavefront,
                               const WavefrontPushConstants &pushConstants);

        /// Executes the miss shader for rays that missed all geometry.
        /// Typically samples environment/sky color.
        void dispatchMissShader(VulkanWaveFrontPipeline *wavefront,
                                const WavefrontPushConstants &pushConstants);

        /// Executes the resolve shader to write final pixel colors.
        /// Accumulates sample contributions to the output image.
        void dispatchResolve(VulkanWaveFrontPipeline *wavefront,
                             const WavefrontPushConstants &pushConstants,
                             uint32_t width, uint32_t height);

        /// Inserts a memory barrier for shader read/write synchronization.
        void insertComputeBarrier();

        /// Inserts a memory barrier after transfer operations (e.g., vkCmdFillBuffer).
        void insertTransferToComputeBarrier();

        /// Inserts a memory barrier for indirect dispatch buffer reads.
        void insertIndirectDispatchBarrier();

        // ============================================================================
        // Member Variables
        // ============================================================================

        VulkanComputeDevice &m_device;
        VkCommandBuffer m_vkCommandBuffer;
        RayTracingPipeline *m_pipeline = nullptr;
        std::vector<VkDescriptorSet> m_descriptorSets;
    };
} // namespace tracey
