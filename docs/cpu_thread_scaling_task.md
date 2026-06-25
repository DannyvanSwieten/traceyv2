# Focused task: fix CPU path-tracer multicore scaling (dynamic work scheduling)

> ## DONE (2026-06-23): shipped, byte-identical, big win
> Implemented the dynamic atomic-cursor scheduler in
> [src/core/parallel.hpp](../src/core/parallel.hpp) (one file). Results on M3 Ultra,
> 512², 4 bounces, full PT, **byte-identical** to the prior output:
> - **DamagedHelmet: 12.0 → 8.6 ms/spp (1.40×)**  (spp=16; 10.9 → 6.5 = 1.68× at spp=32)
> - **Kitchen_set+Dome: 14.1 → 6.2 ms/spp (2.27×)**
> - 32-lane scaling improved from **~7.5× → ~12×**; the go/no-go (finer static
>   chunks) confirmed load imbalance, not bandwidth.
>
> What landed: per-lane static split → many small chunks claimed from a shared
> `atomic<size_t>` cursor by every lane (workers + caller); one wake per dispatch
> via a generation counter (no `notify_all`-per-task, no per-dispatch heap alloc);
> grain = `max(256, n/(lanes*16))`; a `thread_local` reentrancy guard collapses
> nested `parallelFor` to serial (the old queue tolerated nesting; the new
> dispatch-mutex would otherwise deadlock). API unchanged → all ~15 callers
> benefit. Verified byte-identical (helmet + kitchen) and the full smoke suite
> passes (`dop`/POP, `attribute_vop`, `denoiser`, `cloners`, `mograph`,
> `instance_vop`, `usd_import`, `scene_export`). The original spec follows.

**Goal:** make the CPU path tracer scale near-linearly across cores. Today it gets
only **~7.5× on 32 lanes (~24% efficiency)** on an M3 Ultra; the lever is worth
**~2×+ total** — far more than the BVH4 SIMD work (which was tried and gave ~1.02×;
see [bvh4_simd_task.md](bvh4_simd_task.md)). The rendered image must stay
**byte-identical** (it will — see "Why byte-identical").

## Measured baseline (M3 Ultra, DamagedHelmet, 512², 4 bounces, full PT incl. shading)
Scaling sweep (ms/spp, lanes capped via a temporary thread-count knob):

| lanes | 1 | 4 | 16 | 32 |
|---|---|---|---|---|
| ms/spp | 75.4 | 37.3 (2.0×) | 14.1 (5.4×) | 10.0 (7.5×) |

Single-core ≈ **4.9 Mray/s** (incl. shading); 32 lanes ≈ **37 Mray/s aggregate**.
Target: 32-lane ≈ 5–6 ms/spp (~14–15× scaling, ~60–70 Mray/s). For reference, Intel
Embree+TBB scales ~linearly (~85% efficiency); we leave ~3× on the floor.

## Root cause
`ThreadPool::parallelFor` ([src/core/parallel.hpp](../src/core/parallel.hpp)) splits
`[0,n)` into **exactly `workers+1` equal contiguous chunks — one per lane** (static
partition):
```
const size_t chunks    = std::min<size_t>(n, m_workers.size() + 1);
const size_t chunkSize = (n + chunks - 1) / chunks;   // ~8192 contiguous pixels/lane @512²
```
Per-pixel cost is **wildly non-uniform**: a background pixel is one primary ray that
misses; a helmet pixel is 4 bounces + shading + (with lights) shadow rays. So the
lane that draws the all-helmet rows is the straggler and the all-background lanes
finish almost instantly and **idle-wait** on it. That idle wait is the
`__psynch_cvwait` that dominates the profile — it's load imbalance, not raw sync.

Secondary cost: per **dispatch** (= per `render()` = per spp) it takes `m_mutex`,
pushes one task per lane, and `notify_all()`s every worker (thundering herd that
then contends on `m_mutex` to pop). Paid every sample.

## Why byte-identical (the safety argument)
Every `parallel_for_chunks` body processes disjoint index ranges and is already
contractually safe across them. The CPU PT body
([cpu_path_tracer_backend.cpp:430](../src/path_tracer/backends/cpu/cpu_path_tracer_backend.cpp#L430))
writes only `m_accumulator[pixelIdx]` / `m_aovs[k][pixelIdx]` for its own pixels,
and the RNG seed is `px + py*W + globalSampleIdx*W*H` — derived from pixel+sample
coords, **never from the chunk boundaries**. So *any* partition of `[0,n)` yields
the same per-pixel result. Re-chunking changes nothing in the image. **Verify it
anyway** (byte-compare, below) — that's the contract this whole line of work holds.

## Approach (incremental — measure after each step)
Keep the public API **unchanged**: `parallel_for_chunks(n, body, serialThreshold)`
is called from ~15 sites (POP solvers, rasterizer, attribute VOP, render engine,
editor server, PT). The rework is entirely inside `ThreadPool`.

1. **Dynamic chunking (the big win).** Replace the one-chunk-per-lane static split
   with many small chunks claimed from a single shared atomic cursor:
   ```
   std::atomic<size_t> next{0};
   // every lane (workers + caller) runs:
   for (;;) {
       size_t i = next.fetch_add(grain, std::memory_order_relaxed);
       if (i >= n) break;
       body(i, std::min(i + grain, n));
   }
   ```
   This is work-stealing-lite: fast lanes naturally grab more chunks, so background
   vs object imbalance self-balances (and E-cores just take fewer chunks — see
   gotchas). No per-chunk task allocation, no per-chunk mutex. Tune `grain`
   (start ~a few image rows, e.g. 2–8×W pixels, or ~1–4 K items): too small →
   atomic-cursor contention + per-chunk overhead; too large → imbalance returns.

2. **Cheaper dispatch.** One wake per dispatch, not per chunk: hand workers
   `(body-ref, n, grain, generation)` and signal once; they pull from the atomic
   cursor directly. Replace the LIFO task `std::vector` + `notify_all`-per-task with
   a generation counter the workers observe. Consider a short spin before
   condvar-sleep to cut wake latency (dispatch happens every spp, so wake latency
   matters). Completion = an atomic "lanes done" count or a reusable barrier.

3. Keep **caller-participates** (the calling thread is one lane) so a 1-worker
   machine still makes progress and we don't oversubscribe.

A `type-erased Body` (the pool stores `std::function` today) is fine, but the body
is the same for every chunk in a dispatch — store one reference, not one closure
per chunk.

## Files
- [src/core/parallel.hpp](../src/core/parallel.hpp) — **the only file to change.**
  Rework `ThreadPool::parallelFor` + `workerLoop`; leave `parallel_for_chunks` and
  its signature/`serialThreshold` intact so all callers are unaffected.
- No caller changes. The PT benefits at
  [cpu_path_tracer_backend.cpp:430](../src/path_tracer/backends/cpu/cpu_path_tracer_backend.cpp#L430)
  (main render) and `:1042` (post/denoise loop); POP/rasterizer/etc. benefit for free.

## Confirm the bottleneck BEFORE coding (5-minute check)
The fix only pays off if it's imbalance, not memory bandwidth. Quick discriminator:
just make the **static** chunks finer (`chunks = (m_workers.size()+1) * 16`) and
re-run the sweep. If scaling jumps → it was imbalance → do the full dynamic version.
If finer static chunks **don't** help → suspect DRAM bandwidth saturation at high
core counts (all lanes streaming BVH nodes + triangle data + framebuffer); then no
scheduler change helps and the lever moves to data size/bandwidth instead. (The
`__psynch_cvwait` dominance argues for imbalance — idle, not stalled — but confirm.)

## Verification (mandatory)
1. **Byte-identical:** `pt_backend_compare --a cpu --b cpu --bounces 4 --spp 16
   --size 512 --out /tmp/h` then byte-compare `/tmp/h_a.ppm` against a pre-change
   render. Must be identical for both DamagedHelmet and Kitchen_set+Dome.
2. **Scaling:** re-add a temporary lane cap to the `ThreadPool` ctor (an env knob
   like `TRACEY_THREADS` clamping the worker count — there was one during the BVH4
   investigation) or write a microbench, and report ms/spp at 1/4/8/16/24/32 lanes.
   Target ≥0.6 efficiency at 24 lanes (the P-cores). Remove the knob before landing.
3. Build: `cmake --build build/Release --target pt_backend_compare -j8`. The pool is
   in core `tracey`, so the editor + POP/SOP smoke tests rebuild — confirm they pass
   (`usd_import_smoke` / `scene_export_smoke`) since `parallel_for_chunks` is shared.

## Gotchas
- **Memory bandwidth ceiling.** Confirm (above) before assuming the scheduler is the
  whole story; 24–32 cores can saturate DRAM. If bandwidth-bound, scaling caps
  regardless and this task can't reach linear.
- **Apple Silicon P vs E cores.** M3 Ultra = 24 P + 8 E; `hardware_concurrency()`=32
  mixes them. Dynamic chunking self-handles the speed difference (slow E-cores grab
  fewer chunks) — a strong reason dynamic > static here. Optionally set worker QoS to
  `QOS_CLASS_USER_INTERACTIVE` so macOS prefers P-cores, but measure first.
- **False sharing on the framebuffer.** `m_accumulator` is `glm::vec4` (16 B → 4
  pixels/cache line). Keep `grain` ≥ a few rows so two lanes never write the same
  line concurrently; row-aligned grains are cleanest.
- **Atomic-cursor contention.** With grain too small, `next.fetch_add` itself
  becomes a hotspot at 32 lanes. Row-sized grains keep claims rare.
- **Don't change the API.** ~15 callers depend on `parallel_for_chunks(n, body)` and
  the `serialThreshold=1024` inline fallback. Keep both.
- **Determinism unaffected.** No body relies on chunk identity (verified for the PT;
  the contract already forbids it for all callers).
