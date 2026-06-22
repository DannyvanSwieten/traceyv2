#pragma once

// Parallel-for over a range [0, n). Splits the range into contiguous chunks
// run on a process-wide persistent thread pool, each invoking the body with
// its own [begin, end) chunk. Bodies are responsible for their own per-thread
// state (e.g. a slot buffer) — typically a `const` captured variable or one
// constructed inside the chunk — and must be safe to call concurrently across
// disjoint ranges.
//
// `serialThreshold`: below this point count we just run the body inline on the
// calling thread. Dispatching a tiny array to the pool is a net slowdown; the
// default of 1024 mirrors the threshold attribute_vop_sop used to hard-code.
//
// The pool is persistent: workers are created once and reused. Previously each
// call spawned a fresh std::thread per chunk and joined them — fine offline,
// but in the interactive path tracer that was ~one thread-create-and-destroy
// per worker *every frame* (hundreds/sec at 60fps). The pool removes that.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace tracey
{
    // Process-wide fork-join worker pool. Header-only Meyers singleton so there
    // is exactly one instance across all translation units.
    class ThreadPool
    {
    public:
        static ThreadPool &global()
        {
            static ThreadPool instance;
            return instance;
        }

        // Number of worker threads (the calling thread participates too, so the
        // effective width is workerCount()+1 ≈ hardware_concurrency()).
        size_t workerCount() const { return m_workers.size(); }

        // Run body(begin,end) over contiguous chunks of [0,n), blocking until
        // every chunk has completed. The calling thread also processes chunks
        // so it isn't left idle (and a 1-worker machine still makes progress).
        template <typename Body>
        void parallelFor(size_t n, Body &&body)
        {
            if (n == 0) return;

            const size_t chunks = std::min<size_t>(n, m_workers.size() + 1);
            const size_t chunkSize = (n + chunks - 1) / chunks;

            // Build the non-empty chunk ranges. `body` and `remaining` are
            // stack-local here and outlive the tasks (we block until done),
            // so referencing them from the task closures is safe.
            std::vector<std::pair<size_t, size_t>> ranges;
            ranges.reserve(chunks);
            for (size_t c = 0; c < chunks; ++c)
            {
                const size_t begin = c * chunkSize;
                if (begin >= n) break;
                ranges.emplace_back(begin, std::min(n, begin + chunkSize));
            }

            std::atomic<int> remaining{static_cast<int>(ranges.size())};

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const auto &r : ranges)
                {
                    const size_t begin = r.first, end = r.second;
                    m_tasks.emplace_back([this, &body, begin, end, &remaining]() {
                        body(begin, end);
                        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                        {
                            std::lock_guard<std::mutex> g(m_doneMutex);
                            m_doneCv.notify_all();
                        }
                    });
                }
            }
            m_workCv.notify_all();

            // Help drain the queue on the calling thread.
            for (;;)
            {
                std::function<void()> task;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_tasks.empty()) break;
                    task = std::move(m_tasks.back());
                    m_tasks.pop_back();
                }
                task();
            }

            // Wait for any chunks still running on worker threads.
            std::unique_lock<std::mutex> done(m_doneMutex);
            m_doneCv.wait(done, [&] { return remaining.load(std::memory_order_acquire) == 0; });
        }

    private:
        ThreadPool()
        {
            // Reserve a core for the calling thread; the pool + caller then span
            // ~hardware_concurrency() lanes without oversubscribing.
            const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
            const size_t workers = hw > 1 ? hw - 1 : 1;
            m_workers.reserve(workers);
            for (size_t i = 0; i < workers; ++i)
                m_workers.emplace_back([this] { workerLoop(); });
        }

        ~ThreadPool()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_workCv.notify_all();
            for (auto &w : m_workers) w.join();
        }

        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        void workerLoop()
        {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_workCv.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
                    if (m_stop && m_tasks.empty()) return;
                    task = std::move(m_tasks.back());
                    m_tasks.pop_back();
                }
                task();
            }
        }

        std::vector<std::thread> m_workers;
        std::vector<std::function<void()>> m_tasks;  // LIFO; guarded by m_mutex
        std::mutex m_mutex;
        std::condition_variable m_workCv;
        std::mutex m_doneMutex;
        std::condition_variable m_doneCv;
        bool m_stop = false;
    };

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
        ThreadPool::global().parallelFor(n, body);
    }
}
