#include "trace.hpp"
#include "../core/tlas.hpp"
#include <thread>
#include <chrono>
namespace tracey
{
    void traceRays(const UVec2 &resolution, int tileSize, uint32_t iteration, const RaytracerCallback &callback, const Tlas &tlas)
    {
        auto startTime = std::chrono::high_resolution_clock::now();
        const uint numThreads = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        const auto tilesPerRow = (resolution.x + tileSize - 1) / tileSize;
        const auto tilesPerCol = (resolution.y + tileSize - 1) / tileSize;
        const auto totalTiles = tilesPerRow * tilesPerCol;
        std::atomic<int64_t> tilesLeft(totalTiles);

        // Every thread is going to pick up a single tile at a time, So we can keep them small and don't have threads going idle while others are still working.
        const auto threadFunction = [&]()
        {
            const auto totalPixels = (resolution.x / tileSize) * tileSize;
            while (true)
            {
                int64_t tileIndex = tilesLeft.fetch_sub(1, std::memory_order_relaxed);
                if (tileIndex < 0)
                    break;

                const uint32_t tileX = static_cast<uint32_t>(tileIndex) % tilesPerRow;
                const uint32_t tileY = static_cast<uint32_t>(tileIndex) / tilesPerRow;

                const uint32_t baseX = tileX * tileSize;
                const uint32_t baseY = tileY * tileSize;

                for (uint ty = 0; ty < tileSize; ++ty)
                {
                    for (uint tx = 0; tx < tileSize; ++tx)
                    {
                        const uint pixelX = baseX + tx;
                        const uint pixelY = baseY + ty;
                        // Don't call the callback for partially out of bounds tiles
                        if (pixelX < resolution.x && pixelY < resolution.y)
                        {
                            callback(UVec2(pixelX, pixelY), resolution, iteration, tlas);
                        }
                    }
                }
            }
        };

        for (uint t = 0; t < numThreads; ++t)
        {
            threads.emplace_back(threadFunction);
        }

        for (auto &thread : threads)
        {
            thread.join();
        }
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        printf("\nTracing completed in %lld ms\n", duration);
    }
}