# Matrix Multiply — Notes

The same multiply built **five ways**, each attacking a different bottleneck.
Three independent axes turn out to matter, and the variants add them one at a
time:

1. **Access pattern** (loop order) — is memory walked sequentially or strided?
2. **Vectorization** (SIMD) — one `double` per instruction, or four?
3. **Reuse** (cache blocking) — how many times is each byte re-fetched from RAM?

This writeup follows the investigation: measure, read the assembly, work out
*why*, fix it, confirm on the real binary — and finally, learn to measure
without lying to yourself.

## What the benchmark does

Computes `C = A * B` for square `N x N` matrices of `double`, row-major. Each
matrix is a contiguous `N*N` block in a `std::vector`, viewed through a
`std::span`; since `span` is 1-D I index the row-major layout by hand —
element `(row i, col k)` is at `A[i*N + k]`. (`std::mdspan` would give 2-D
`A[i,k]` indexing but libstdc++ 15 doesn't ship it yet.)

The metric is **GFLOPS**. A multiply-add is 2 FLOPs and the algorithm does
`N^3` of them, so each call is `2*N^3` FLOPs — directly comparable across
variants and independent of `N`, so every size sits on the same y-axis. The
`N` sweep crosses the cache levels: the working set is three matrices,
`3 * N*N * 8` bytes.

| N | one matrix | three matrices | vs cache (L1d 32 KB / L2 256 KB / L3 6 MB) |
|---|---|---|---|
| 64   | 32 KB  | 96 KB  | one matrix = L1d; all three fit L2 |
| 128  | 128 KB | 384 KB | three spill past L2 |
| 256  | 512 KB | 1.5 MB | each ≫ L2, all fit L3 |
| 512  | 2 MB   | 6 MB   | at the L3 ceiling |
| 1024 | 8 MB   | 24 MB  | ≫ L3 — into RAM |
| 2048 | 32 MB  | 96 MB  | deep into RAM |

Naive is only swept to N=512 — it's `O(N^3)` and gets painfully slow; the fast
variants go to 2048 to reach the out-of-cache regime.

## How these numbers were measured

Getting *stable, honest* numbers turned out to be its own lesson. The canonical
run is:

```
taskset -c 3 setarch -R ./build/matrix_multiply \
  --benchmark_repetitions=5 --benchmark_min_time=2s \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=results/matrix_multiply/canonical.json --benchmark_out_format=json
```

Each flag kills a specific source of noise:

- **`setarch -R`** (ASLR off) — fixes the memory layout so cache-set mapping and
  page alignment don't shift run to run.
- **`taskset -c 3`** — pins to one core so the scheduler can't migrate the
  thread and hand it a cold per-core L1/L2.
- **`--benchmark_repetitions=5`** — turns each point into a mean + stddev instead
  of a single lucky/unlucky shot. Matters most at large N, where one benchmark
  "iteration" already takes seconds and Google Benchmark can't internally average.
- **`--benchmark_min_time=2s`** — forces several iterations per repetition even at
  large N. Without it, the `1024` point ran a *single* ~0.66 s iteration (just
  over the 0.5 s default) and its stddev blew up to ~12%; with it, ~0.2%.
- **`performance` governor**, quiet machine (close the browser — a background tab
  at 30% CPU produced 40% stddev and non-monotonic points).

Canonical run stddev: **≤2.4%**, median 0.3%. The one caveat is **turbo**: this
is a thermally-limited laptop, so the clock floats a little across a multi-minute
run. The canonical numbers are at full turbo (representative of normal
operation); see the clock-speed aside at the end for what pinning the clock does.

---

## Canonical result

All five variants, GFLOPS by matrix size (full turbo, canonical run):

| N | naive (ijk) | ikj | + restrict | + simd | tiled |
|---|-------------|-----|------------|--------|-------|
| 64   | 2.74 | 4.75 | 14.53 | 13.63 | 13.27 |
| 128  | 2.33 | 4.21 | 12.57 | 12.25 | 12.07 |
| 256  | 1.40 | 3.89 | 11.35 | 11.14 | 11.00 |
| 512  | 1.32 | 3.29 |  9.66 |  9.53 |  9.79 |
| 1024 |  —   | 2.57 |  5.00 |  4.97 |  9.17 |
| 2048 |  —   | 2.95 |  3.60 |  3.59 |  8.05 |

![Matrix multiply — GFLOPS vs working-set size](../results/matrix_multiply/canonical.png)

Two things set the shape. **Vectorization sets the ceiling**: the three SIMD
variants (restrict, simd, tiled) start ~3× above the scalar ones and sit on top
of one another while the data is cache-resident. **Tiling defends that ceiling
past the cache cliff**: at the L3 line (6 MB, N=512) restrict and simd fall away
as they re-stream the B-matrix from RAM, while tiled holds ~8–9 GFLOPS — ~2.2× at N=2048.
The rest of this note is how each variant gets there.

---

## Variant 1: naive `ijk` (the baseline)

```cpp
for i: for j: { sum = 0; for k: sum += A[i*N+k] * B[k*N+j]; C[i*N+j] = sum; }
```

The inner loop fixes `i` and `j` and varies `k`. `A[i*N+k]` walks **along a row**
of A (stride 1). `B[k*N+j]` walks **down a column** of B (stride N) — every
access a different cache line, and all of B re-traversed for every row of C.
That column walk is the strided pathology, and it is why this version is slow.

| N | GFLOPS |
|---|--------|
| 64  | 2.74 |
| 128 | 2.33 |
| 256 | 1.40 |
| 512 | 1.32 |

The downward curve is the cache cliff: while B fits a fast cache the column
re-walks are cheap; as B spills L2 → L3 → RAM each re-walk pays more latency.

**Assembly.** Scalar throughout. With `-march=native` the inner loop is a single
fused multiply-add per element:

```asm
.L4: vmovsd      xmm1, [rax]              ; load A[i*N+k]
     vfmadd231sd xmm0, xmm1, [rdx]        ; sum += A * B[k*N+j]  (FMA, scalar)
     add         rdx, rcx                 ; rdx += N*8 -> stride-N column walk on B
```

`vfmadd231sd` is a fused multiply-add, but the `sd` suffix means **scalar** — one
double per instruction, not the four-wide `pd` (packed) form we want. The inner
loop is a **reduction** (many products summed into one `sum`), and vectorizing a
reduction means keeping several partial sums in parallel and combining them at the
end — which **regroups the additions**. Floating-point `+` rounds at every step,
so it is not associative: `(a+b)+c` and `a+(b+c)` can differ in the last bits.
That regrouping would change the result, so GCC keeps the exact left-to-right
order by default and only vectorizes the reduction if `-ffast-math` is passed.

## Variant 2: `ikj` (cache-friendly reorder)

```cpp
for i: for k: { r = A[i*N+k]; for j: C[i*N+j] += r * B[k*N+j]; }
```

Reorder to `i,k,j` and hoist `A[i*N+k]` into a scalar `r`. Now the inner loop
over `j` walks `B[k][*]` and `C[i][*]` **along rows** (stride 1) — sequential,
cache-friendly. Same `N^3` multiply-adds, just a memory-friendlier order. `C` is
built with `+=` across `k`, so it must start zeroed (the fixture `std::fill`s it).

| N | naive | ikj | speedup |
|---|-------|-----|---------|
| 64  | 2.74 | 4.75 | 1.7× |
| 128 | 2.33 | 4.21 | 1.8× |
| 256 | 1.40 | 3.89 | 2.8× |
| 512 | 1.32 | 3.29 | 2.5× |

The speedup **grows with N**: at small N both fit cache so the edge is small; at
large N naive thrashes RAM while ikj streams B sequentially. Sequential access
uses all 8 doubles per 64-byte line where the column walk used 1 of 8.

**Assembly.** Still scalar — but *not* because of FP math. Each `C[j]` is
independent (the vectorizable shape), so the blocker is **aliasing**: A, B, C
arrive as spans (just pointers), and the compiler can't prove `C` doesn't overlap
`B`. If they overlapped, doing 4 at a time could read a value it just wrote — so
it stays scalar to be safe. The whole ~2.5× here is the **cache** win; zero SIMD.

## Variant 3: `ikj + __restrict` (auto-vectorized SIMD)

Same loop, but the kernel takes raw `const double* __restrict` pointers.
`__restrict` promises A, B, C never overlap — which removes the aliasing doubt
and lets the compiler auto-vectorize.

| N | ikj | restrict | speedup |
|---|-----|----------|---------|
| 64  | 4.75 | 14.53 | 3.1× |
| 128 | 4.21 | 12.57 | 3.0× |
| 256 | 3.89 | 11.35 | 2.9× |
| 512 | 3.29 |  9.66 | 2.9× |

**Assembly.** Now the inner loop uses `ymm` registers and `vfmadd*pd` (`pd` =
packed double, 4 at a time), plus a scalar remainder loop for the last `0–3`
elements. The ~3× is roughly flat across N — a SIMD-width win (AVX2 packs 4
doubles), independent of where the data lives.

```asm
vmovupd     ymm0, [B...]
vfmadd213pd ymm0, ymm1, [C...]   ; 4 FMAs at once; C folded as memory operand
vmovupd     [C...], ymm0
```

(A `static` single-caller function gets **inlined** and vanishes from the `.o`;
`[[gnu::noinline]]` keeps it a standalone symbol to read in `objdump`.)

## Variant 4: `ikj + explicit SIMD` (`std::experimental::simd`)

Same vectorization, but written **by hand** with `std::experimental::simd`
instead of relying on the auto-vectorizer: broadcast `r` into a vector, loop `j`
in steps of `V::size()` (4), load a B-chunk, load a C-chunk, `vc += vr*vb`, store
back, then a scalar remainder for the tail.

The interesting part is what it *doesn't* cost: because **we** emit the vector
loads/stores, the aliasing doubt no longer blocks anything, so this keeps the
**safe `std::span` signature** and still vectorizes — no raw `__restrict`
pointers. The aliasing guarantee didn't disappear; it moved onto the programmer.

| N | restrict | simd (explicit) |
|---|----------|-----------------|
| 64  | 14.53 | 13.63 |
| 256 | 11.35 | 11.14 |
| 512 |  9.66 |  9.53 |
| 2048 | 3.60 |  3.59 |

They tie — within run-to-run noise. And they should: the disassembly of the two
inner loops is the **same three instructions** (`vmovupd` / `vfmadd213pd [mem]` /
`vmovupd`). The explicit-SIMD `V vc(&C[…])` load even gets folded into the FMA's
memory operand, so 4 written lines compile to 3 instructions, not 4. So the value
of this variant isn't speed — it's keeping spans while getting SIMD.

(`std::experimental::simd` is the Parallelism TS v2; the real `std::simd` lands
in C++26 with a slightly different spelling. Concepts transfer 1:1.)

## The three gates of FP auto-vectorization

Each variant isolates one gate:

| Gate | What blocks it | Unlock | Seen in |
|------|----------------|--------|---------|
| **ISA** | CPU features not targeted | `-march=native` | scalar `mulsd/addsd` → fused `vfmadd*` |
| **Math** | FP reduction (associativity) | `-ffast-math` | naive `ijk`'s `sum +=` stays scalar |
| **Aliasing** | can't prove no overlap | `__restrict` / explicit SIMD | ikj span → scalar; restrict → packed |

Almost every real "why won't this loop vectorize?" is one of these three.

## Access pattern and SIMD are orthogonal

| N | naive | ikj | restrict | ikj/naive | restrict/ikj | restrict/naive |
|---|-------|-----|----------|-----------|--------------|----------------|
| 64  | 2.74 | 4.75 | 14.53 | 1.7× | 3.1× | 5.3× |
| 256 | 1.40 | 3.89 | 11.35 | 2.8× | 2.9× | 8.1× |
| 512 | 1.32 | 3.29 |  9.66 | 2.5× | 2.9× | 7.3× |

The **cache fix (ikj)** attacks the *memory access pattern* — its win grows with
N. The **SIMD fix (restrict)** attacks the *compute/instruction count* — its win
is flat. Different bottlenecks, different fixes, so they **stack**: ~5–8× over
naive. But notice restrict/simd still slide downward as N grows — they fix how
*fast* each pass streams, not how *many* passes there are. That's the next wall.

---

## Variant 5: `tiled` (cache blocking) — attacking reuse

restrict and simd hit a ceiling and then fall off it at large N. At N=2048 they
drop to ~3.6 GFLOPS — because they still **re-stream the whole B-matrix from RAM for every
row of C**. There's no *reuse*. That's the last bottleneck, and it needs a
different idea: **blocking**.

### The reuse model

Run the multiply over `T x T` blocks small enough to stay cache-resident, so a
loaded block is used many times before it's evicted. The key quantity is how many
times all of B is read from RAM:

- **Untiled:** compute C one full row at a time. B doesn't fit cache, so it's
  evicted between rows → B is swept from RAM **N times** (once per row).
- **Tiled (edge T):** process **T rows together**; a loaded B-block serves all T
  of them back-to-back while it's hot → B swept from RAM **N/T times**.

So RAM traffic is **∝ 1/T**. Concretely, B's bytes read from RAM ≈ `8·N³/T`:

```
N=2048:   T=32  → ~2.1 GB     T=256 → ~270 MB   (8× less)
```

`T` is the *reuse factor*: each fetched element does ~`2T` FLOPs of work before
eviction, and total work is fixed, so more work-per-fetch means fewer fetches.
The reuses only count as cache *hits* if the block is still resident — which is
what keeping the block small guarantees. (Analogy: a tiny backpack loads fast but
forces 8 trips to the store; the bottleneck is the driving — RAM — not the
packing.)

### The kernel

Six loops: three outer (`ii, jj, kk`) stepping by `T` over blocks, three inner
running the same `ikj` multiply over the block, clamped with `min(ii+T, N)` for
edge tiles / small N. It keeps the `__restrict` inner kernel, so **it still
auto-vectorizes** — tiling stacks *on top of* SIMD, it doesn't replace it
(confirmed in the disassembly: same `ymm`/`vfmadd213pd`).

### The result: crossover at the cache cliff

| N | ikj | restrict | simd | **tiled** |
|---|-----|----------|------|-----------|
| 256  | 3.89 | 11.35 | 11.14 | 11.00 |
| 512  | 3.29 |  9.66 |  9.53 |  9.79 |
| 1024 | 2.57 |  5.00 |  4.97 |  **9.17** |
| 2048 | 2.95 |  3.60 |  3.59 |  **8.05** |

Below the cliff (N ≤ 512, data cache-resident) tiling is pure overhead and just
ties restrict/simd — there are no RAM trips to save. Past the cliff (N ≥ 1024) it
is the **only** variant that changes how *many* times B is fetched, so it holds
~8–9 GFLOPS while the others halve and halve again. At N=2048, **~2.2× restrict**.

The canonical plot above tells the story at a glance. Plotting GFLOPS against
**working-set bytes** (same x-axis and cache lines as the rest of the suite)
makes the mechanism visible: restrict/simd dive **exactly at the L3 line
(6 MB)**, and tiled diverges from them at precisely that point. The cliff *is*
the working set outgrowing last-level cache.

### Choosing T

`T` is chosen in **bytes against a cache level**, and the footprint is 2-D:
`3 * T^2 * 8` bytes for the three blocks. Bigger T = more reuse, but it must still
fit a cache level with headroom.

| T | 3-block footprint | fits | reuse |
|---|-------------------|------|-------|
| 32  | 24 KB  | L1 | ~32× — *weakest* of the fitting sizes |
| 128 | 384 KB | ~L2 | ~128× |
| 256 | 1.5 MB | L3 (with room) | ~256× — **best here** |
| 512 | 6 MB   | = L3 exactly | eviction thrash |
| 1024| 24 MB  | fits nothing | back to RAM-bound |

Exploratory sweep (single-run, turbo), tiled GFLOPS:

| T | N=1024 | N=2048 |
|---|--------|--------|
| 32  | ~5.9 | ~4.8 |
| 128 | ~7.2 | ~7.0 |
| 256 | ~8.3 | ~8.2 |

The counter-intuitive lesson: **L1-blocking (T=32) is the *worst* fitting
choice**, not the best. "Fits L1" isn't the goal — reuse-per-RAM-fetch (∝ T) is,
and the L1-tile captures the least of it. The best choice is the **largest** T
whose triple still fits *a* cache. Past that (T=512 at the L3 boundary, T=1024 overflowing
everything, or degenerating into no tiling at all) it collapses. `T=256` is the
sweet spot on this machine. Cache-hit latency (L1 4 cyc vs L3 40 cyc) is noise
next to the RAM misses (200+ cyc) that T controls.

---

## A note on clock speed (bonus)

The tiling margin depends on the CPU clock. Re-running at a **fixed base clock**
(turbo off, ~1.6 GHz) vs **full turbo** (~3.6 GHz):

| @ N=2048 | full turbo | base clock |
|----------|-----------|-----------|
| tiled | 8.05 | 3.45 |
| restrict | 3.60 | 2.28 |
| **tiling margin** | **2.2×** | **1.5×** |

![Tiling margin at two clocks](../results/matrix_multiply/clock_comparison.png)

The gap between tiled and restrict is **wider at full turbo**. Why: a faster CPU
outruns RAM *harder*, so it's *more* memory-bound, so the reuse win matters more.
Slow the compute down and the machine becomes relatively less memory-starved, and
tiling's advantage shrinks. Same idea as a roofline: which optimization pays off
depends on where the workload sits relative to the memory bandwidth ceiling — and that
position moves with clock speed. (Turbo-off is also the *cleaner* measurement —
fixed clock never trips the thermal limit — but it measures an unrepresentative
operating point, so the canonical run above uses full turbo.)

## Hardware

Intel Core i7-8550U (Kaby Lake R), 4 cores / 8 threads, 1.8 GHz base / 4.0 GHz
boost (observed ~3.6 GHz sustained under load — thermally limited). L1d 32 KB per
core, L2 256 KB per core, L3 6 MB shared. `-O2 -march=native`, GCC 15,
libstdc++ 15. See [hardware_background.md](hardware_background.md) for the full
port/cache reference.

## Takeaways

- **Loop order is a memory decision.** ijk vs ikj is identical arithmetic; the
  only difference is whether B is walked down a column (strided) or along a row
  (sequential). ~2.5×.
- **`__restrict` is worth ~3× and `std::span` can't express it** — no no-alias
  promise, no auto-SIMD. `std::experimental::simd` recovers SIMD *while* keeping
  spans, by taking on the aliasing guarantee yourself; it ties restrict because
  it compiles to the same hot loop.
- **Stack independent optimizations.** Cache pattern and SIMD are orthogonal, so
  their wins multiply (~5–8× over naive).
- **Tiling attacks reuse, the last bottleneck.** RAM traffic ∝ 1/T; pick the
  largest tile whose `3·T²·8` footprint still fits a cache level. It only wins
  past the cache cliff, but there it's the *only* thing that works — ~2× at
  N=2048, erasing the memory wall.
- **Measurement is a skill.** Pin the core, disable ASLR, repeat, raise
  `min_time`, quiet the machine, and know whether turbo is helping or distorting
  the result. The story only became trustworthy once the stddev dropped under a percent.
- **Verify at the instruction level.** Every claim here — scalar vs packed, the
  simd/restrict tie, tiling still vectorizing — was confirmed by reading the
  actual `.o`, not just the timings.

## References

- **Source:** [`src/matrix_multiply.cpp`](../src/matrix_multiply.cpp)
- **Results:** [`results/matrix_multiply/canonical.json`](../results/matrix_multiply/canonical.json)
  (full turbo), [`no_turbo.json`](../results/matrix_multiply/no_turbo.json) (base clock)
- **Plots:** [`canonical.png`](../results/matrix_multiply/canonical.png),
  [`clock_comparison.png`](../results/matrix_multiply/clock_comparison.png)
  (generated by [`scripts/plot_results.py`](../scripts/plot_results.py) and
  [`scripts/plot_clock_comparison.py`](../scripts/plot_clock_comparison.py))
- **Related:** [`strided.md`](strided.md) (B's column walk is the same strided
  access), [`aos_vs_soa.md`](aos_vs_soa.md) (spatial-locality waste).
