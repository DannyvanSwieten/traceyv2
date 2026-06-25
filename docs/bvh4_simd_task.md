# Focused task: 4-wide BVH (BVH4) for the CPU path tracer

> ## OUTCOME (2026-06-23): attempted, INEFFECTIVE — do not re-attempt as specced
>
> BVH4 was fully implemented and **verified byte-identical** (the collapse + the
> NEON 4-box test below all work, on an Apple M3 Ultra). It delivered **~1.02×,
> not 1.5–2×.** The premise — that the box-test *math* dominates and widening it
> to 4 lanes will compound the single-box NEON win — does not hold on this
> hardware. Measured, then reverted. Evidence:
>
> - **BVH4 (4-wide box test):** helmet 12.0→11.9 ms/spp, Duck 3.59→3.55. ~noise.
> - **Collapse was healthy** (9963 BVH2 nodes → 2452 BVH4, 1031 full 4-wide), so
>   it was not a build bug.
> - **Not memory-bound:** cache-resident Duck showed no win either (if it were
>   node-fetch latency, the small scene would have surfaced the compute win).
> - **Not sort-bound:** dropping the near-far child sort made it *slower*.
> - **Triangle-data locality** (slot-ordered leaf copy, so the leaf loop reads
>   sequentially instead of gathering `m_triangleData[primId]`): also a wash
>   (~0% helmet). Mesh index-order is locally coherent, so the "gather" wasn't
>   actually missing cache.
>
> **Why:** the single-box NEON `intersectAABB` (commit `7405174`) already took the
> easy SIMD win. The remaining traversal cost is the **latency-bound pointer-chase**
> (pop → load node → test → branch → push), which neither a wider box test nor a
> reordered triangle array touches. Ray packets would help coherent rays but the
> doc correctly rules them out for incoherent GI bounces.
>
> ### Where the time actually goes (profiled, CPU backend, M3 Ultra)
> Of compute self-time: `Blas::intersect` (box test + traversal glue) ~51%,
> `intersectTriangle` ~15%, shading ~24%, `Tlas::intersect` ~5%. So intersection
> dominates — but it's the *glue*, not the SIMD-able math.
>
> ### The real lever: multicore scaling, not BVH SIMD
> Single-core helmet ≈ **4.9 Mray/s** (full path tracing incl. shading; ~372k
> rays/spp at 75 ms/spp). 32 lanes → **~37 Mray/s aggregate** at ~10 ms/spp, i.e.
> only **~7.5× on 32 lanes (~24% efficiency)**. The thread pool (mutex + condvar
> + `notify_all` per `render()` dispatch) is the bottleneck — `__psynch_cvwait`
> dominates the profile. Fixing scaling toward ~linear is worth **~2×+**, far more
> than any BVH-width change, and is the recommended next perf work.
> The one remaining byte-identical SIMD lever is the 4-triangle leaf test *done
> right* with FMA-matched intrinsics (see "Already done" below) — realistic ~5%.

**Goal (original, NOT MET):** make the CPU path-tracer backend ~1.5–2× faster via
a 4-wide BVH whose box tests use all 4 NEON lanes, **without changing the rendered
image** (the box test must stay bit-identical to the scalar path).

**Why this is the win:** the box *traversal* dominates the CPU tracer (the leaf
triangle test is a smaller fraction — measured ~5% from SIMD-ing it). A binary
BVH (BVH2) only fills 2 of NEON's 4 lanes; a 4-wide BVH tests 4 child boxes in one
`float32x4` pass — the ideal NEON fit. Context: the user wants a genuinely fast CPU
backend so "pick CPU or GPU" is practical (both backends are always selectable; the
"identical CPU/GPU pixels" idea was set aside — it's infeasible across HW-RT vs BVH,
see memory `cpu-metal-pt-divergence`).

## Already done (do NOT redo)
- **NEON `intersectAABB`** (commit `7405174`): single-box slab test, 4-wide, ~5–10%,
  verified **bit-identical**. In `src/core/intersect.hpp`. Stayed bit-identical
  because the slab test is `(bmin−o)*inv` then min/max — no `a*b−c*d` to FMA-contract.
- **SIMD 4-triangle leaf: TRIED AND REVERTED.** ~5% only, and `-O3` FMA-contracts the
  scalar `glm::cross`/`dot` while explicit `vmulq`+`vsubq` don't → diverged ~2% of
  pixels (max 36/255 at edges). Poor trade. **Keep the triangle leaf test scalar.**
  If you ever revisit it, you must match FMA contraction (use `vfmaq`/`vfmsq` or
  `#pragma clang fp contract`) to stay bit-identical.

## Approach: collapse BVH2 → BVH4 (reuse the proven builder)
Do NOT write a new SAH builder. Build the binary BVH as today (`Blas::buildRecursive`
/ the TLAS equivalent), then **post-process into 4-wide nodes**: each BVH4 node holds
up to 4 children = the (up to 4) grandchildren of a BVH2 interior node (collapse two
binary levels). Children that are leaves or don't exist get a sentinel.

- New 4-wide node (SoA box layout for NEON): `float bmin_x[4], bmin_y[4], bmin_z[4],
  bmax_x[4], bmax_y[4], bmax_z[4]; uint32_t child[4]; uint8_t childCount;` (and a
  leaf/interior tag per child, or reuse the existing leaf encoding).
- Keep the leaves exactly as the BVH2 produces them (scalar triangle test, see above).

## NEON 4-box traversal (the bit-identical core)
Mirror `intersectAABB` but 4-wide across the 4 children's SoA bounds:
```
t0 = (bmin[axis] - o[axis]) * inv[axis]   // one float32x4 per axis, 3 axes
t1 = (bmax[axis] - o[axis]) * inv[axis]
tEnter = max(min(t0x,t1x), min(t0y,t1y), min(t0z,t1z), minT)   // per lane
tExit  = min(max(t0x,t1x), max(t0y,t1y), max(t0z,t1z), maxT)
hitMask = (tExit >= tEnter) & (tEnter < closestT) & (lane < childCount)
```
Then visit hit children **near-to-far** (sort the ≤4 tEnter values; push far-first
onto the stack so nearest pops first — same ordering the BVH2 code uses). Use only
`vsub/vmul/vmin/vmax` + horizontal reduces (no `a*b−c*d`) so it stays bit-identical.

## Files
- `src/core/bvh_node.hpp` — add the BVH4 node struct.
- `src/core/blas.{hpp,cpp}` — collapse pass + `Blas::intersect` BVH4 traversal. **Start here**
  (dominant for dense meshes like DamagedHelmet).
- `src/core/tlas.{hpp,cpp}` — same collapse + traversal for the instance BVH. Do AFTER
  the BLAS works (dominant for instanced scenes like Kitchen_set).
- `src/core/intersect.hpp` — reuse/extend the NEON helpers.
- Keep a scalar `#else` fallback for non-`__ARM_NEON` targets.

## Verification (mandatory, per step)
The CPU output must stay **byte-identical** to the current scalar/BVH2 result.
Harness: `examples/pt_backend_compare` (built target `pt_backend_compare`), which now
has per-backend timing, `--dome <i>`, and `--sun <i>`.

1. Bit-identical check: render CPU twice — once on `main`/pre-change, once after — and
   byte-compare the PPM. Method used before: `--a cpu --b cpu ... --out X` then diff
   `X_a.ppm`. They MUST be identical (the box test has no FMA pattern, so this is
   achievable). If they differ, you introduced an `a*b−c*d` contraction mismatch.
2. Speed: the harness prints `[cpu] N spp in M ms (ms/spp)`. Warm baselines AFTER
   `intersectAABB` (512², 4 bounces, 16 spp): **Kitchen_set+Dome ~14 ms/spp,
   DamagedHelmet ~12 ms/spp**. Target ~1.5–2× faster.
3. Commands:
   - `pt_backend_compare --a cpu --b cpu --dome 1 --bounces 4 --spp 16 --size 512 ~/Downloads/Kitchen_set/Kitchen_set.usd --out /tmp/k`
   - `pt_backend_compare --a cpu --b cpu --bounces 4 --spp 16 --size 512 --out /tmp/h` (DamagedHelmet default)
4. Build: `cmake --build build/Release --target pt_backend_compare -j8`. Full editor:
   `cmake --build build/Release -j8` (the BVH is in core `tracey`, so the editor + all
   smoke tests rebuild — confirm `usd_import_smoke` / `scene_export_smoke` still pass).

## Gotchas
- **FMA contraction** is the bit-identicality trap (see the reverted leaf). Box tests
  avoid it; if you add any `a*b−c*d`, verify byte-equality immediately.
- The BVH is used by the CPU path tracer (and any CPU picking/raycast). A bug breaks
  rendering — verify at each step.
- `leafThreshold = 4` (`BVHConfig`); `BVHNode` is 32 bytes (binary). The collapse must
  preserve the leaf encoding (`primCountAndType`, `firstChildOrPrim`) so leaves are
  untouched.
- The traversal stack is `StackEntry stack[64]` — a BVH4 is shallower, so 64 is ample.
- **Skip ray packets.** They amortize traversal only for coherent rays; path-tracer GI
  bounces are incoherent, so packets add masking complexity for little gain here.
