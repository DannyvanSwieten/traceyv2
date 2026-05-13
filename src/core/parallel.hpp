#pragma once

// Parallel-for over a range [0, n). Splits the range across
// std::thread::hardware_concurrency() workers, each invoking the body
// with its own [begin, end) chunk. Bodies are responsible for their
// own per-thread state (e.g. a slot buffer) — typically a `const`
// captured variable or one constructed inside the chunk.
//
// `serialThreshold`: below this point count we just run the body
// inline on the calling thread. Starting a few hundred microseconds
// of threads to operate on a tiny array is a net slowdown; the
// default of 1024 mirrors the threshold attribute_vop_sop used to
// hard-code.

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

namespace tracey
{
    // Body signature: void(size_t begin, size_t end) — half-open range.
    template <typename Body>
    inline void parallel_for_chunks(size_t n, Body body, size_t serialThreshold = 1024)
    {
        if (n == 0) return;
        if (n < serialThreshold)
        {
            body(size_t{0}, n);
            return;
        }
        const size_t numThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
        const size_t chunkSize  = (n + numThreads - 1) / numThreads;

        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (size_t t = 0; t < numThreads; ++t)
        {
            const size_t begin = t * chunkSize;
            const size_t end   = std::min(n, begin + chunkSize);
            if (begin >= end) break;
            threads.emplace_back(body, begin, end);
        }
        for (auto &th : threads) th.join();
    }
}
