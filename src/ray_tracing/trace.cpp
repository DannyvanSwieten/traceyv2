#include "trace.hpp"
#include "../core/tlas.hpp"
#include <thread>
namespace tracey
{
    void traceRays(const UVec2 &resolution, const RaytracerCallback &callback, const Tlas &tlas)
    {
        const uint numThreads = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        const auto totalPixels = resolution.x * resolution.y;
        std::atomic<int64_t> pixelsLeft(totalPixels);

        // Every thread is going to pick up a single pixel at a time, So we don't have thread going idle while others are still working.
        const auto threadFunction = [&]()
        {
            while (true)
            {
                const auto pixelIndex = pixelsLeft.fetch_sub(1);
                if (pixelIndex <= 0)
                    break;

                UVec2 location(pixelIndex % resolution.x, pixelIndex / resolution.x);
                callback(location, resolution, tlas);

                float progess = (totalPixels - pixelsLeft.load()) / static_cast<float>(totalPixels);
                if (pixelIndex % 1000 == 0)
                {
                    printf("\rProgress: %.2f%%", progess * 100.0f);
                    fflush(stdout);
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
    }
}