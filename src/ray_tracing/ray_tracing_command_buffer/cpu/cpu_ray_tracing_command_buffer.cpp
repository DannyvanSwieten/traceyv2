#include "cpu_ray_tracing_command_buffer.hpp"
#include "../../ray_tracing_pipeline/cpu/cpu_ray_tracing_pipeline.hpp"
#include "../../ray_tracing_pipeline/cpu/cpu_descriptor_set.hpp"
#include "../../ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../../../device/buffer.hpp"
#include <cassert>
#include <chrono>
#include <thread>
namespace tracey
{
    CpuRayTracingCommandBuffer::CpuRayTracingCommandBuffer()
    {
    }
    void CpuRayTracingCommandBuffer::begin()
    {
    }

    void CpuRayTracingCommandBuffer::end()
    {
    }

    void CpuRayTracingCommandBuffer::setPipeline(RayTracingPipeline *pipeline)
    {
        m_pipeline = dynamic_cast<CpuRayTracingPipeline *>(pipeline);
    }
    void CpuRayTracingCommandBuffer::setDescriptorSet(DescriptorSet *set)
    {
        m_descriptorSet = dynamic_cast<CpuDescriptorSet *>(set);
        const auto &layout = m_pipeline->layout();
        for (size_t i = 0; i < layout.bindings().size(); ++i)
        {
            const auto &binding = layout.bindings()[i];
            const auto index = layout.indexForBinding(binding.name);
            if (binding.type == RayTracingPipelineLayoutDescriptor::DescriptorType::AccelerationStructure)
            {
                // Set the pipeline pointer in the DispatchedTlas
                m_descriptorSet->visitMut([this](auto &&arg)
                                          {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, DispatchedTlas >)
                    {
                        arg.pipeline = m_pipeline;
                    } },
                                          index);
            }
        }
    }
    void CpuRayTracingCommandBuffer::traceRays(const ShaderBindingTable &sbt, uint32_t width, uint32_t height)
    {
        if (!m_pipeline)
        {
            assert(false && "Pipeline not set for CpuRayTracingCommandBuffer");
            return;
        }

        std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();

        auto compiledSbt = m_pipeline->compiledSbt();
        const auto &layout = m_pipeline->layout();
        for (const auto &binding : layout.bindings())
        {
            size_t index = layout.indexForBinding(binding.name);
            // Binding setup logic would go here
            if (binding.stage == ShaderStage::RayGeneration)
            {
                // Setup for ray generation shader
                m_descriptorSet->visitMut([&](auto &&arg)
                                          {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, Image2D *>)
                    {
                        *m_pipeline->compiledSbt().rayGen.shader.bindingSlots[index].slotPtr = arg;
                    }
                    else if constexpr (std::is_same_v<T, Buffer *>)
                    {
                        // Setup buffer
                        *m_pipeline->compiledSbt().rayGen.shader.bindingSlots[index].slotPtr = arg;
                    }
                    else if constexpr (std::is_same_v<T,  DispatchedTlas>)
                    {
                        *m_pipeline->compiledSbt().rayGen.shader.bindingSlots[index].slotPtr = &arg;
                    } },
                                          index);
            }
            else if (binding.stage == ShaderStage::ClosestHit)
            {
                // Setup for hit shaders
                m_descriptorSet->visitMut([&](auto &&arg)
                                          {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, Image2D *>)
                    {
                        *m_pipeline->compiledSbt().hits[0].shader.bindingSlots[index].slotPtr = arg;
                    }
                    else if constexpr (std::is_same_v<T, Buffer *>)
                    {
                        // Setup buffer
                        *m_pipeline->compiledSbt().hits[0].shader.bindingSlots[index].slotPtr = arg->mapForWriting();
                    }
                    else if constexpr (std::is_same_v<T,  DispatchedTlas>)
                    {
                        *m_pipeline->compiledSbt().hits[0].shader.bindingSlots[index].slotPtr = &arg;
                    } },
                                          index);
            }
        }

        std::vector<std::thread> threads;

        // 2D tiling for better locality
        constexpr int64_t tileW = 16;
        constexpr int64_t tileH = 16;
        const int64_t tilesX = (static_cast<int64_t>(width) + tileW - 1) / tileW;
        const int64_t tilesY = (static_cast<int64_t>(height) + tileH - 1) / tileH;
        const int64_t totalTiles = tilesX * tilesY;

        // Atomic tile index (counting down)
        std::atomic<int64_t> nextTile = totalTiles;

        for (size_t i = 0; i < std::thread::hardware_concurrency(); ++i)
        {
            threads.emplace_back([&]()
                                 {
                // Each thread processes tiles. Avoid mutating shared SBT state.
                void* payloadPtr = nullptr;
                if (!compiledSbt.rayGen.shader.payloadSlots.empty() && compiledSbt.rayGen.shader.payloadSlots[0])
                {
                    payloadPtr = std::malloc(compiledSbt.rayGen.shader.payloadSlots[0]->payloadSize);
                    compiledSbt.rayGen.shader.payloadSlots[0]->setPayload(&payloadPtr, 0);
                    compiledSbt.hits[0].shader.payloadSlots[0]->setPayload(&payloadPtr, 0);
                    compiledSbt.misses[0].shader.payloadSlots[0]->setPayload(&payloadPtr, 0);
                }

                // Reuse builtins per thread to reduce per-pixel overhead.
                rt::Builtins builtins;
                builtins.glLaunchSizeEXT = {width, height, 1};

                for (;;)
                {
                    // fetch next tile index (counting down)
                    int64_t tileIndex = nextTile.fetch_sub(1, std::memory_order_relaxed) - 1;
                    if (tileIndex < 0)
                        break;

                    const int64_t tileX = tileIndex % tilesX;
                    const int64_t tileY = tileIndex / tilesX;

                    const uint32_t baseX = static_cast<uint32_t>(tileX * tileW);
                    const uint32_t baseY = static_cast<uint32_t>(tileY * tileH);

                    // Iterate pixels inside the tile
                    for (uint32_t localY = 0; localY < static_cast<uint32_t>(tileH); ++localY)
                    {
                        const uint32_t py = baseY + localY;
                        if (py >= height)
                            break;

                        for (uint32_t localX = 0; localX < static_cast<uint32_t>(tileW); ++localX)
                        {
                            const uint32_t px = baseX + localX;
                            if (px >= width)
                                break;

                            if (compiledSbt.rayGen.shader.func)
                            {
                                builtins.glLaunchIDEXT = {px, py, 0};
                                if (compiledSbt.rayGen.setBuiltins)
                                {
                                    compiledSbt.rayGen.setBuiltins(builtins);
                                }
                                compiledSbt.rayGen.shader.func();
                            }
                        }
                    }
                } });
        }

        for (auto &thread : threads)
        {
            thread.join();
        }

        std::chrono::high_resolution_clock::time_point endTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> duration = endTime - startTime;
        printf("Ray tracing completed in %.2f ms\n", duration.count());
    }
    void CpuRayTracingCommandBuffer::copyImageToBuffer(const Image2D *image, Buffer *buffer)
    {
        // TODO: Implement image to buffer copy for CPU ray tracing
    }
    void CpuRayTracingCommandBuffer::waitUntilCompleted()
    {
    }
}