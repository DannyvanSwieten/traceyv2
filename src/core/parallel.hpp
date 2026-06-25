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
//
// Scheduling is DYNAMIC: [0,n) is divided into many small chunks that every lane
// (workers + the calling thread) claims from a shared atomic cursor as it goes.
// A previous version handed each lane one equal contiguous chunk, which load-
// imbalanced badly when per-item cost varies (path-tracer background vs object
// pixels): the heavy lane straggled while the rest idled (~7.5× on 32 cores).
// Pulling small chunks from the cursor self-balances (~14× on 32 cores) and lets
// slower cores (e.g. Apple E-cores) simply claim fewer chunks.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace tracey
{
    // Brief CPU pause hint, used while spin-waiting for the next job (workers) or
    // for the worker lanes to drain (caller) before falling back to a condvar.
    // Far cheaper than a futex round-trip when the next dispatch is imminent —
    // the common case while the path tracer accumulates samples back-to-back.
    static inline void cpuRelax()
    {
#if defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }

    // Spin iterations before sleeping. Bounded, so an idle pool spins once and
    // then sleeps (no continuous core burn), but long enough to catch a
    // back-to-back dispatch without paying a futex wake.
    static constexpr int kPoolSpinIters = 8192;

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

        // Run body(begin,end) over small dynamic chunks of [0,n), blocking until
        // all are done. Every lane (workers + the calling thread) pulls chunks
        // from a shared atomic cursor, so work self-balances and a 0-worker
        // machine still makes progress on the caller.
        template <typename Body>
        void parallelFor(size_t n, Body &&body)
        {
            if (n == 0) return;

            const size_t lanes = m_workers.size() + 1; // workers + caller
            const size_t grain = chunkGrain(n, lanes);

            // Nested dispatch (a body that itself calls parallelFor) or a
            // 0-worker pool: run the chunks inline on this thread. Nesting must
            // NOT re-enter the pool — a lane re-locking m_dispatchMutex would
            // deadlock — so inner parallel-fors collapse to serial, exactly as
            // the old queue-based pool effectively behaved.
            if (m_workers.empty() || t_inJob)
            {
                for (size_t i = 0; i < n; i += grain)
                    body(i, std::min(n, i + grain));
                return;
            }

            // Trampoline so workers can invoke `body` through a void* without a
            // per-dispatch heap allocation (vs storing a std::function). `body`
            // lives on the caller's stack and outlives the dispatch (we block
            // until every lane finishes below).
            using BodyT = std::remove_reference_t<Body>;
            JobFn fn = [](void *ctx, size_t b, size_t e) {
                (*static_cast<BodyT *>(ctx))(b, e);
            };

            // One job at a time on the shared pool (fork-join).
            std::lock_guard<std::mutex> dispatch(m_dispatchMutex);

            // Publish the job, then bump the generation to release the workers.
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_jobFn = fn;
                m_jobCtx = static_cast<void *>(std::addressof(body));
                m_jobN = n;
                m_jobGrain = grain;
                m_cursor.store(0, std::memory_order_relaxed);
                m_lanesRemaining.store(static_cast<int>(m_workers.size()),
                                       std::memory_order_relaxed);
                // Release: a worker that observes this generation via a lock-free
                // acquire-load (spin path) is guaranteed to see the job fields +
                // reset cursor/lanes written above.
                m_generation.fetch_add(1, std::memory_order_release);
            }
            m_workCv.notify_all();

            // The calling thread is a lane too.
            runJob();

            // Wait for the worker lanes to drain the cursor for this generation.
            // Spin first (they're finishing their last chunks — usually within a
            // chunk-time), then fall back to the condvar so we don't burn a core
            // if a straggler runs long.
            bool drained = false;
            for (int i = 0; i < kPoolSpinIters; ++i)
            {
                if (m_lanesRemaining.load(std::memory_order_acquire) == 0) { drained = true; break; }
                cpuRelax();
            }
            if (!drained)
            {
                std::unique_lock<std::mutex> done(m_doneMutex);
                m_doneCv.wait(done, [this] {
                    return m_lanesRemaining.load(std::memory_order_acquire) == 0;
                });
            }
        }

    private:
        using JobFn = void (*)(void *, size_t, size_t);

        ThreadPool()
        {
            // Reserve a core for the calling thread; the pool + caller then span
            // ~hardware_concurrency() lanes without oversubscribing.
            const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
            const size_t workers = hw > 1 ? hw - 1 : 0; // caller is one lane
            m_workers.reserve(workers);
            for (size_t i = 0; i < workers; ++i)
                m_workers.emplace_back([this] { workerLoop(); });
        }

        ~ThreadPool()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop.store(true, std::memory_order_release);
            }
            m_workCv.notify_all();
            for (auto &w : m_workers) w.join();
        }

        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        // Chunk size: aim for ~kChunksPerLane chunks per lane so a slow lane only
        // delays the others by at most one chunk, but keep chunks coarse enough
        // that the atomic cursor and per-chunk overhead stay negligible (and
        // adjacent lanes don't share an output cache line).
        static size_t chunkGrain(size_t n, size_t lanes)
        {
            constexpr size_t kChunksPerLane = 16;
            constexpr size_t kMinGrain = 256;
            const size_t target = lanes * kChunksPerLane;
            return std::max<size_t>(kMinGrain, (n + target - 1) / target);
        }

        // Claim and run chunks from the shared cursor until [0,n) is exhausted.
        // While a body runs, this lane is "in a job", so a nested parallelFor it
        // triggers collapses to serial instead of re-entering (and deadlocking on)
        // the pool.
        void runJob()
        {
            const size_t n = m_jobN, grain = m_jobGrain;
            const JobFn fn = m_jobFn;
            void *const ctx = m_jobCtx;
            const bool prevInJob = t_inJob;
            t_inJob = true;
            for (;;)
            {
                const size_t i = m_cursor.fetch_add(grain, std::memory_order_relaxed);
                if (i >= n) break;
                fn(ctx, i, std::min(n, i + grain));
            }
            t_inJob = prevInJob;
        }

        void workerLoop()
        {
            uint64_t lastGen = 0;
            for (;;)
            {
                // Spin briefly for the next generation before sleeping: catches a
                // back-to-back dispatch without a futex wake. Bounded, so an idle
                // pool spins once then sleeps on the condvar (which still wakes it
                // on the next dispatch — the predicate covers a gen bump that
                // lands between the spin ending and the wait starting).
                bool gotWork = false;
                for (int i = 0; i < kPoolSpinIters; ++i)
                {
                    if (m_stop.load(std::memory_order_acquire)) return;
                    if (m_generation.load(std::memory_order_acquire) != lastGen) { gotWork = true; break; }
                    cpuRelax();
                }
                if (!gotWork)
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_workCv.wait(lock, [this, lastGen] {
                        return m_stop.load(std::memory_order_relaxed) ||
                               m_generation.load(std::memory_order_relaxed) != lastGen;
                    });
                    if (m_stop.load(std::memory_order_relaxed)) return;
                }
                lastGen = m_generation.load(std::memory_order_acquire);
                runJob();
                if (m_lanesRemaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    std::lock_guard<std::mutex> g(m_doneMutex);
                    m_doneCv.notify_all();
                }
            }
        }

        std::vector<std::thread> m_workers;
        std::mutex m_mutex;               // guards job publication + m_generation
        std::condition_variable m_workCv; // wakes workers on a new generation
        std::mutex m_doneMutex;
        std::condition_variable m_doneCv; // signals the caller when lanes drain
        std::mutex m_dispatchMutex;       // serializes dispatches (one job at a time)

        // Current job (published under m_mutex, read lock-free after a lane has
        // observed the matching generation; stable until the next dispatch, which
        // m_dispatchMutex + the done-wait prevent from overlapping).
        JobFn m_jobFn = nullptr;
        void *m_jobCtx = nullptr;
        size_t m_jobN = 0;
        size_t m_jobGrain = 1;
        std::atomic<size_t> m_cursor{0};
        std::atomic<int> m_lanesRemaining{0};
        // Atomic so worker lanes can poll them lock-free during the spin phase
        // (the condvar paths still hold m_mutex). m_generation is bumped with
        // release on dispatch; m_stop with release on teardown.
        std::atomic<uint64_t> m_generation{0};
        std::atomic<bool> m_stop{false};

        // True while this thread is executing a chunk body, so a nested
        // parallelFor on the same thread runs serially instead of re-entering.
        static inline thread_local bool t_inJob = false;
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
