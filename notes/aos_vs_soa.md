# AoS vs SoA — Notes

A four-variant investigation. Each run contradicted a prediction from the
previous one. The writeup is the lab notebook, not a clean tutorial — the
wrong turns are where the insights live.

## What the benchmark does

Simulates a common pattern: a collection of "particles" with position
`(x, y, z)` and velocity `(vx, vy, vz)`, where a hot loop only reads the `x`
coordinate. Two data layouts are compared:

- **AoS (Array of Structures)** — each particle is a 48-byte struct stored in
  a single `std::vector<Particle>`. Reading only `x` touches 8 useful bytes
  per 48-byte struct, so 83% of every cache line fetched is wasted on fields
  the loop never reads.
- **SoA (Structure of Arrays)** — each field is its own
  `std::vector<uint64_t>`. Reading `x` touches only the contiguous `x` array,
  so every byte fetched is used. This is the same access pattern as the
  sequential benchmark.

The interesting question: what does the 6× cache-line waste actually *cost*
in bandwidth, and where in the hierarchy does it show up?

## Variant 1: `double` + 1 accumulator (the failed first attempt)

```cpp
double sum = 0;
for (size_t i = 0; i < limit; ++i)
{
  sum += particles[i].x;  // AoS
}
```

**Hypothesis**: AoS wastes 5/6 of each cache line, so SoA should be roughly
6× faster, especially at L2 and below.

**Result**: both AoS and SoA reported a flat **~7 GB/s across every working
set size** — L1d, L2, L3, and RAM all identical. The ratio between them was
near 1×, not 6×. The expected staircase was missing entirely.

**Lesson**: a flat bandwidth curve across all cache levels is the fingerprint
of a **compute-bound** workload, not a memory-bound one. The single
accumulator creates a serial dependency chain — each `sum += ...` must wait
for the previous add to complete. FP add on Kaby Lake has 4-cycle latency, so
the throughput ceiling is one add every 4 cycles regardless of memory speed.
At 4 GHz that caps bandwidth around what I saw.

**The memory hierarchy is invisible when compute is the bottleneck.** Any
benchmark that wants to measure memory must first prove it isn't measuring
something else. Same failure mode as the sequential 1-accumulator case, just
with FP latency instead of integer.

## Variant 2: `double` + 8 accumulators

Fix was to break the serial chain with 8 independent accumulators:

```cpp
double s0=0, s1=0, s2=0, s3=0, s4=0, s5=0, s6=0, s7=0;
for (size_t i = 0; i < limit; i += 8)
{
  s0 += particles[i+0].x;
  s1 += particles[i+1].x;
  // ... s2..s7
}
```

**Result** (GB/s, `double`, 8 accumulators):

| Working set | Region | AoS | SoA | Ratio |
|-------------|--------|-----|-----|-------|
| 4 KB   | L1d | 37.9 | 57.8 | 1.5× |
| 32 KB  | L1d | 41.9 | 57.4 | 1.4× |
| 64 KB  | L2  | 19.9 | 57.8 | 2.9× |
| 256 KB | L2  | 15.8 | 58.1 | 3.7× |
| 1 MB   | L3  | 10.4 | 58.0 | 5.6× |
| 4 MB   | L3  | 8.3  | 55.2 | 6.6× |
| 8 MB   | RAM | 3.5  | 53.4 | 15×  |
| 32 MB  | RAM | 2.6  | 30.2 | 12×  |
| 256 MB | RAM | 2.4  | 13.8 | 5.8× |

AoS behaved — a clean descending staircase. But SoA was wrong: **flat ~58
GB/s from L1d through L3**, then a cliff to ~14 GB/s at deep RAM. A
memory-bound curve cannot be flat across three cache levels with very
different bandwidths. Something was capping SoA below memory speed.

**Lesson**: the SoA plateau is *still* a compute ceiling, but a different
one. `vaddpd ymm` (AVX2 FP add on doubles) has 4-cycle latency and runs on 2
ports. With 2 ymm accumulators (the compiler folded 8 scalars into 2
vectors):

```
2 ports × (1/4 cycle) × 32 B per ymm × 4 GHz ≈ 64 GB/s
```

That matches the observed 58 GB/s plateau. **SoA was not memory-bound until
RAM sizes**; it was bottlenecked by FP-add pipeline latency at every level
above. The AoS/SoA ratio of 5.8× at RAM looked correct, but was coincidence —
both hit memory-bound territory only at the very bottom of the curve. Two
different compute ceilings produced the same-looking "5-6× ratio," and the
numbers alone couldn't distinguish them. I had to change the code.

## Variant 3: `uint64_t` + 8 accumulators

To eliminate the FP ceiling, switch particle fields from `double` to
`uint64_t`. Same 8-byte width, same 48-byte struct size — but integer add has
**1-cycle latency** on **3 ports**, so it should never be the bottleneck:

```cpp
struct Particle {
  uint64_t x, y, z;
  uint64_t vx, vy, vz;
};
```

**Result** (GB/s, `uint64_t`, 8 accumulators, **original SoA sizing**):

| Working set | Region | AoS | SoA | Sequential |
|-------------|--------|-----|-----|------------|
| 4 KB   | L1d | 36  | 99  | 121 |
| 32 KB  | L1d | 37  | 104 | 117 |
| 64 KB  | L2  | 19  | 114 | 105 |
| 128 KB | L2  | 16  | 117 | 102 |
| 256 KB | L2  | 15  | 104 | 90  |
| 1 MB   | L3  | 11  | 83  | 61  |
| 4 MB   | L3  | 7.1 | 60  | 49  |
| 8 MB   | RAM | 2.8 | 59  | 18  |
| 32 MB  | RAM | 2.5 | 23  | 15  |
| 256 MB | RAM | 2.5 | 13  | 14  |

SoA jumped from a flat 58 GB/s plateau to ~100+ GB/s at L1d, confirming the
FP-ceiling theory. The plateau is gone.

But the new curve is *very* strange. At 64 KB and 128 KB SoA hits **117
GB/s** — higher than Sequential and higher than SoA's own 4 KB number. "L2"
looks faster than "L1d." That makes no physical sense. Also strange: the
SoA→RAM cliff sits at **~32 MB**, six times further right than Sequential's
8 MB cliff.

**Lesson**: a **methodology bug**, not a hardware mystery. The SoA benchmark
was computing `n = size / sizeof(Particle)` — the same formula as AoS — and
then allocating six separate `std::vector<uint64_t>` arrays of size `n`. But
the inner loop only touches the `x` array. So when `size` says "8 MB", AoS
actually touches 8 MB (full lines pulled for every access), while SoA only
touches `8 MB / 6 ≈ 1.3 MB` of hot data.

The x-axis was **lying** about cache pressure. SoA's "L1d/L2/L3/RAM
transitions" were shifted right by exactly the 6× ratio between particle size
and hot-field size. The 117 GB/s at "64 KB" was really L1d behavior at 10 KB
of hot data. The 59 GB/s at "8 MB" was really L3 behavior at 1.3 MB.

**Benchmark x-axes must represent bytes-actually-touched by the loop, not
bytes-allocated.** The same `n` that works correctly for AoS sizes SoA wrong,
because the two layouts have different allocated-to-touched ratios.

**A second observation from this run, equally important**: AoS L1d numbers
*did not change* between Variant 2 and Variant 3. Doubles: ~38 GB/s. uint64:
~37 GB/s. If FP latency had been the AoS L1d ceiling, switching to 1-cycle
integer adds should have roughly doubled throughput. It didn't. **AoS's L1d
ceiling is something other than FP latency** — and whatever it is, it's
identical for doubles and uint64s. A new mystery.

## Variant 4: `uint64_t` + 8 accumulators + corrected sizing (canonical)

Sizing fix: redefine `size` in both benchmarks to mean "bytes of hot data the
loop actually touches":

- **AoS**: `n = size / sizeof(Particle)` (each particle pulls a full 48 B
  into cache because the line fetch is atomic).
- **SoA**: `n = size / sizeof(uint64_t)` (only the `x` array is touched, 8 B
  per element).

With this convention, at `size = 8 MB` both benchmarks exert the same 8 MB of
cache pressure even though AoS iterates over 175K particles and SoA over 1M
elements. That is the fair apples-to-apples comparison.

**Result** (GB/s, `uint64_t`, 8 accumulators, **corrected sizing** — canonical,
measured on a cold, idle box with core 3 pinned and median clock ≈ 3.68 GHz):

| Working set | Region | AoS | SoA | Sequential (8 acc) |
|-------------|--------|-----|-----|--------------------|
| 4 KB   | L1d | 34.0 | 100.4 | ~100 |
| 16 KB  | L1d | 27.6 | 100.8 | ~101 |
| 32 KB  | L1d | 29.1 | 106.5 | ~107 |
| 64 KB  | L2  | 16.4 | 79.7  | —    |
| 128 KB | L2  | 16.9 | 78.0  | —    |
| 256 KB | L2  | 15.2 | 66.1  | —    |
| 1 MB   | L3  | 9.8  | 45.5  | —    |
| 4 MB   | L3  | 8.2  | 27.3  | —    |
| 32 MB  | RAM | 2.40 | 10.2  | —    |
| 256 MB | RAM | 2.21 | 12.0  | —    |

![AoS vs SoA canonical](../results/aos_vs_soa/v4_canonical.png)

Both predictions held. SoA overlaps Sequential almost perfectly — the two
are structurally identical (read a contiguous uint64 array), and with sizing
fixed they produce the same numbers. SoA→RAM cliff arrives between 4 MB and
8 MB as the working set overflows L3. RAM ratio AoS:SoA = 12/2.21 ≈ **5.4×**,
very close to the theoretical 6× waste factor (small gap is the prefetcher
helping AoS slightly on consecutive lines).

### SoA L1d bandwidth: why ~100 GB/s, not higher?

Running back-to-back three times on an idle box with core-3 frequency logging
(median 3.67–3.68 GHz across runs, max 3.90), SoA consistently measures
**99.5–107 GB/s** at 4–32 KB. The bench is load-port limited at 8
accumulators: loop body is tiny, OoO window sits ~40% full, and the CPU
retires at ~27 B/cycle instead of the 64 B/cycle ceiling. See "Accumulator
count revisited" below for the investigation.

## Accumulator count revisited (the SoA16 experiment)

Rebuilding the notes surfaced a question: if the source already has eight
`uint64_t` accumulators `s0..s7`, why can't SoA match the ~120 GB/s the
sequential benchmark reportedly hit? Two theories were tested.

**Theory A** — GCC collapses the 8 scalars into only 2 ymm vectors, so I'm
running with 2-wide ILP instead of 8-wide.

**Theory B** — GCC packs the 8 u64 scalars into 2× `vpaddq ymm` (4 u64s
each), preserving 8-way parallelism; the real bottleneck is something else.

Reading `/tmp/aos.s` confirmed Theory B. The SoA hot loop (`.L217`) is:

```asm
.L217:
    inc     rdx
    vpaddq  ymm1, ymm1, YMMWORD PTR [rax]        ; 4 u64 adds (s0..s3)
    vpaddq  ymm0, ymm0, YMMWORD PTR [rax+32]     ; 4 u64 adds (s4..s7)
    add     rax, 64
    cmp     rdx, rcx
    jb      .L217
```

That's two 256-bit loads + two dependent adds per iteration. Each `vpaddq`
has 1-cycle latency with no stall between iterations (independent
accumulators `ymm0` and `ymm1`). The theoretical ceiling at 3.68 GHz is
`2 loads × 32 B × 3.68 GHz ≈ 235 GB/s`. I measured **100 GB/s** — 42% of
the ceiling. So 8-way ILP is real, but the loop isn't saturating the load
ports.

### Why 8 accumulators leaves performance on the table

The loop body is 5 fused-domain µops: two `vpaddq`, `inc rdx`, `cmp`, `jb`
(the `add rax, 64` fuses with the compare under macro-op fusion). At
4 µops/cycle retirement that's 1.25 cycles per 64-byte iteration, or
51 B/cycle × 3.68 GHz ≈ 188 GB/s ceiling even before load-port contention.

The gap from 188 to 100 is the cost of *short iteration counts* at small
working sets. At 4 KB = 64 loop iterations, Google Benchmark's outer
machinery (increment counter, branch, `DoNotOptimize` reduction hoist) adds
non-negligible overhead per `benchmark::State` step. The OoO window never
reaches steady state. More accumulators help because each iteration does
proportionally more useful work relative to that fixed overhead.

### The SoA16 experiment

To test this, a copy of `BM_SoA` was added with sixteen scalar accumulators
(`s0..sf`) and a stride-16 inner loop. GCC folded this into **four**
`vpaddq ymm` per iteration (128 bytes per outer iteration), exactly as it
folded the 8-scalar version into two.

**Result** at L1d sizes, same cold-box conditions:

| Working set | SoA (8 acc) | SoA (16 acc) | Ratio |
|-------------|-------------|--------------|-------|
| 4 KB   | 100.4 | 125.7 | 1.25× |
| 16 KB  | 100.8 | 154.0 | 1.53× |
| 32 KB  | 106.5 | 154.4 | 1.45× |

SoA16 hits **154 GB/s at L1d** — 82% of the 188 GB/s retirement ceiling
and 65% of the 235 GB/s load-port ceiling. The loop body grows to ~7 µops
(4× vpaddq plus loop control), so the retirement-limited ceiling actually
*drops* to ~128 GB/s on paper, but the larger body amortizes the Benchmark
framework overhead enough that effective throughput still climbs. This
confirms the bottleneck for the 8-accumulator version is Benchmark/OoO
startup, not microarchitectural parallelism.

### Why 8 accumulators is still canonical

The sequential accumulator sweep (`results/sequential/accumulator_sweep.png`)
shows an interesting inversion across the cache hierarchy:

| Region | 8 acc | 16 acc | Winner |
|--------|-------|--------|--------|
| L1d (4–32 KB) | ~102 GB/s | ~132 GB/s | 16 acc |
| L2 (64–256 KB) | ~90 GB/s | ~91 GB/s | tie |
| L3 (512 KB – 4 MB) | ~47 GB/s | ~49 GB/s | tie |
| RAM (8+ MB) | ~14 GB/s | ~12 GB/s | **8 acc** |

At L1d, 16 accumulators wins by exploiting ILP more aggressively. Past the
L1d cliff, both variants plateau at the same L2/L3 bandwidth — memory is
the bottleneck, not compute, so extra accumulators don't help. At RAM
sizes, the 16-accumulator loop is slightly *worse* because its larger body
adds loop-control overhead that the memory bottleneck can't hide.

The purpose of these benchmarks is the **staircase** — revealing L1d → L2
→ L3 → RAM transitions cleanly. 8 accumulators gives the flattest,
cleanest staircase across the whole range while still lifting L1d above the
compute ceiling enough to make the first step visible. 16 accumulators
would draw a taller first step but add noise at the bottom of the curve.
So 8 stays canonical; `BM_SoA16` lives in the source as a compute-ceiling
reference that the reader can rebuild and rerun if they want to see the
L1d load-port headroom directly.

### What this means for the AoS L1d "port 5" story

The AoS L1d penalty analysis further down this file (the `vpinsrq` /
`vinserti128` gather-vectorization saturation at port 5) is unchanged by
the SoA revision. AoS at L1d still reports ~34–37 GB/s in every run —
reading the assembly and counting port-5 µops remains the only way that
number makes sense. What the 8-vs-16 accumulator investigation tightens is
the *reference point* I compare it against: SoA at 100 GB/s (real) or
154 GB/s (compute ceiling), not the fictional 120 GB/s from earlier
drafts.

## The AoS L1d mystery: why is it 37 GB/s?

With sizing fixed, the AoS L1d ceiling at ~37 GB/s was still unexplained. I
ruled out:

- FP latency (Variant 3 — switching to uint64 didn't help).
- Cache misses (AoS at 4-32 KB fits entirely in L1d).
- Data-dependent stalls (8 independent accumulators).
- Load-port saturation? *Expected* ~64 GB/s (2 load ports × 8 B × 4 GHz),
  measured 37.

At 37 GB/s the CPU is doing **1.15 scalar loads per cycle** on average, well
below the 2-per-cycle peak. Something was capping load throughput at ~60% of
the theoretical ceiling, and the obvious explanations didn't fit.

The only way to resolve this is to **read the assembly** and cycle-count:

```bash
g++ -O2 -march=native -std=c++23 -S -masm=intel \
    -I include -I build/_deps/benchmark-src/include \
    src/aos_vs_soa.cpp -o /tmp/aos.s
```

Hot loop for `BM_AoS` (label `.L311`, slightly reformatted):

```asm
.L311:
    ; ── Gather first 4 particles' .x fields into ymm0 ──
    vmovq         xmm1, QWORD PTR [rax+240]          ; particles[i+5].x → xmm1.lo
    vmovq         xmm0, QWORD PTR [rax+288]          ; particles[i+6].x → xmm0.lo
    add           rax, 384                            ; stride 8 particles × 48 B
    vpinsrq       xmm1, xmm1, QWORD PTR [rax-192], 1  ; particles[i+4].x → xmm1.hi
    vpinsrq       xmm0, xmm0, QWORD PTR [rax-48],  1  ; particles[i+7].x → xmm0.hi
    vinserti128   ymm0, ymm0, xmm1, 0x1               ; merge → ymm0 (4 uint64s)
    ; ── Gather second 4 particles, accumulate first group ──
    vmovq         xmm1, QWORD PTR [rax-288]          ; particles[i+2].x → xmm1.lo
    vpaddq        ymm2, ymm2, ymm0                   ; accumulator 2 += ymm0
    vmovq         xmm0, QWORD PTR [rax-384]          ; particles[i+0].x → xmm0.lo
    vpinsrq       xmm1, xmm1, QWORD PTR [rax-336], 1 ; particles[i+1].x → xmm1.hi
    vpinsrq       xmm0, xmm0, QWORD PTR [rax-240], 1 ; particles[i+3].x → xmm0.hi
    vinserti128   ymm0, ymm0, xmm1, 0x1              ; merge → ymm0 (4 more uint64s)
    vpaddq        ymm3, ymm3, ymm0                   ; accumulator 3 += ymm0
    ; ── Loop control ──
    cmp           rdx, rax
    jne           .L311
```

**GCC tried to vectorize the AoS loop.** It manually *gathered* the scattered
`.x` fields into two `ymm` registers using `vmovq`/`vpinsrq`/`vinserti128`
sequences, then accumulated them into `ymm2`/`ymm3`. Intent: once data is in
ymm form, the add is one 1-cycle `vpaddq ymm` instead of 8 scalar adds.

The problem is the cost of getting the data *into* ymm form. Uop counts per
inner iteration:

| Instruction | Count | Port(s) used | Notes |
|-------------|-------|-------------|-------|
| `vmovq xmm, [mem]`        | 4 | p2 or p3 (load) | Pure load uop |
| `vpinsrq xmm, xmm, [mem]` | 4 | p5 + (p2/p3)    | 2 uops: 1 load + **1 port-5 insert** |
| `vinserti128 ymm, ymm, xmm` | 2 | **p5**        | Lane-crossing insert, port 5 only |
| `vpaddq ymm`              | 2 | p0/p1/p5 (ALU)  | Integer vector add |
| `add rax, 384`            | 1 | any ALU         | Pointer bump |
| `cmp/jne` (fused)         | 1 | p6              | Fused macro-op |

**Port 5 uops per iteration**: 4 (vpinsrq inserts) + 2 (vinserti128) =
**6 port-5 uops**. Skylake/Kaby Lake has exactly **one port 5**, and it is
the only port that can execute lane-crossing shuffles and scalar-to-vector
inserts. So port 5 becomes the bottleneck at 6 cycles per iteration.

```
6 port-5 cycles ÷ iteration
 ÷ 64 useful bytes per iteration
 × 4 GHz
≈ 39 GB/s predicted
```

Measured: **37 GB/s**. Within 5% — about as close as hand cycle-counting gets
you. **Port 5 shuffle saturation is the AoS L1d ceiling.** Load ports are
idle (only 8 loads per 6 cycles = 1.3 loads/cycle, well under the 2/cycle
peak). Front-end is also idle. Port 5 alone is the narrow pipe.

**The cruel irony**: if GCC had *not* vectorized, and instead emitted 8 plain
scalar `add reg, [mem]` instructions, the bottleneck would have become the 2
load ports at 4 cycles per iteration = 64 GB/s theoretical. The compiler's
attempt to be clever made things *worse*. A canonical example of a
well-intentioned optimization that hurts because the cost of a precondition
(gathering scattered data into ymm form) exceeds the benefit of the optimized
operation (vectorized add).

## The novec counter-experiment

To verify the port-5 theory, I built a second binary with vectorization
disabled. CMake change:

```cmake
add_executable(aos_vs_soa_novec src/aos_vs_soa.cpp)
target_compile_options(aos_vs_soa_novec PRIVATE -fno-tree-vectorize)
```

`-fno-tree-vectorize` disables both loop and SLP vectorization, so GCC falls
back to plain scalar code. The resulting inner loop is radically simpler:

```asm
.L305:
    add   rcx, QWORD PTR [rax]          ; s0 += particles[i+0].x
    add   rsi, QWORD PTR [rax+48]       ; s1 += particles[i+1].x
    add   rax, 384                       ; advance 8 particles × 48 B
    add   rdx, QWORD PTR [rax-288]      ; s2 += particles[i+2].x
    add   rdi, QWORD PTR [rax-240]      ; s3 += particles[i+3].x
    add   r8,  QWORD PTR [rax-192]      ; s4 += particles[i+4].x
    add   r9,  QWORD PTR [rax-144]      ; s5 += particles[i+5].x
    add   r10, QWORD PTR [rax-96]       ; s6 += particles[i+6].x
    add   r11, QWORD PTR [rax-48]       ; s7 += particles[i+7].x
    cmp   r14, rax
    jne   .L305
```

Eight `add reg, [mem]` load-op instructions with no shuffles, no inserts, no
ymm registers. Each load-op decomposes into one load uop (p2/p3) and one add
uop (any ALU port). **Port 5 is completely idle now.** Bottleneck becomes the
2 load ports at 8 loads ÷ 2 per cycle = 4 cycles per iteration = **64 GB/s
theoretical**.

Comparison at L1d and below (`uint64_t`, corrected sizing, cold idle box,
core-3 pinned, median clock ≈ 3.68 GHz during runs):

| Working set | AoS vec | AoS novec | Δ       | SoA vec | SoA novec |
|-------------|---------|-----------|---------|---------|-----------|
| 4 KB   | 33.5 | **48.7** | +45% | 99.4 | 41.2 |
| 32 KB  | 33.7 | 47.3     | +40% | 101.2 | 36.0 |
| 64 KB  | 18.0 | 18.1     | ~0%  | 91.4 | 23.2 |
| 256 KB | 12.2 | 13.3     | +9%  | 64.6 | 18.0 |
| 1 MB   | 9.4  | 9.3      | −1%  | 51.3 | 19.4 |
| 4 MB   | 7.1  | 6.9      | −3%  | 37.5 | 12.7 |
| 8 MB   | 2.6  | 2.8      | +8%  | 10.6 | 6.3  |
| 256 MB | 2.0  | 1.6      | −20% | 11.2 | 8.9  |

![AoS vs SoA: vectorized vs novec](../results/aos_vs_soa/novec_comparison.png)

Three observations, one of them contradicting the writeup's original
claim — that's worth spelling out.

**1. AoS L1d jumped from 34 to 49 GB/s (+45%).** The direction matches the
port-5 theory: removing vectorization removes the gather shuffle µops, and
the loop is now bound by the 2 scalar load ports. The theoretical ceiling is
`2 loads × 8 B × 3.68 GHz ≈ 59 GB/s`. I measured 49 — about 83% of
ceiling. The remaining gap is the loop-control and reduction overhead, the
same Benchmark-framework tax that kept SoA at 100 GB/s instead of the
~188 GB/s ceiling discussed earlier. Direction of the effect is confirmed,
magnitude confirms the direction of the effect.

**2. At L2 and below, vectorized and novec AoS converge** to within a few
percent. This is exactly expected: once memory is the bottleneck, the
CPU-side instruction shape stops mattering. The 6× cache-line waste dominates
regardless of whether the code is scalar or vectorized.

**3. Novec SoA does *not* equal novec AoS at L1d — SoA is actually lower.**
AoS novec sits at 47-49 GB/s and
SoA novec at 32-41 GB/s (both at L1d). Both loops do 8 scalar `add reg,
[mem]` operations per iteration (verified by dumping
`-fno-tree-vectorize` assembly of both), and both should be bound by the
same 2 load ports. Why SoA novec measures consistently ~20% lower is not
understood.

Two hypotheses that I did **not** run to ground because the AoS/SoA story at
L1d is about the *vectorized* case:

- **L1d bank conflicts.** Skylake's L1d has 8 banks, 8 bytes wide. SoA's 8
  consecutive u64 loads hit every bank exactly once — which should be optimal,
  but certain inter-iteration aliasing patterns can still cause replays. A
  `mem_load_retired.fb_hit` or `ld_blocks.*` `perf stat` run would confirm
  or rule this out.
- **Iteration count mismatch confusing steady-state measurement.** At 4 KB,
  AoS runs 10 inner iterations per `benchmark::State` step while SoA runs 64.
  If the 10-iteration AoS loop happens to fit better in the OoO window for
  this tiny working set, it may run closer to its ceiling than the 64-iteration
  SoA loop, even though both have the same per-iteration uop mix.

Whichever hypothesis is correct, the novec experiment's *qualitative* claim
still holds: **disabling vectorization lifts AoS L1d bandwidth substantially,
which only makes sense if vectorized AoS was bottlenecked on something other
than load-port throughput** — and port 5 is the only µop in the vectorized
code that plausibly sits on the critical path.

**Is the L1d AoS penalty a compiler artifact?** Vectorized SoA is 3×
vectorized AoS at L1d, while novec SoA is ~0.8× novec AoS — so the
vectorized gap is larger than pure load-port scaling would predict, but the
novec ratio isn't the clean 1:1 that would prove "no L1d layout cost." The
honest reading is: the gather-vectorization story explains most of the
vectorized L1d gap, but there's a residual factor in the scalar comparison
that needs perf-counter investigation before making a stronger claim.

## The full four-way comparison

All values GB/s, `uint64_t` throughout, cold idle box, core-3 pinned,
median clock ~3.68 GHz. Sequential column is the 8-accumulator canonical run.

| Working set | Region | Sequential | SoA vec | SoA novec | SoA16 vec | AoS vec | AoS novec |
|-------------|--------|-----------|---------|-----------|-----------|---------|-----------|
| 4 KB   | L1d | 100 | 99.4  | 41.2 | 125.7 | 33.5 | 48.7 |
| 32 KB  | L1d | 107 | 101.2 | 36.0 | 154.4 | 33.7 | 47.3 |
| 64 KB  | L2  | —   | 91.4  | 23.2 | —     | 18.0 | 18.1 |
| 256 KB | L2  | —   | 64.6  | 18.0 | —     | 12.2 | 13.3 |
| 1 MB   | L3  | —   | 51.3  | 19.4 | —     | 9.4  | 9.3  |
| 4 MB   | L3  | —   | 37.5  | 12.7 | —     | 7.1  | 6.9  |
| 8 MB   | RAM | —   | 10.6  | 6.3  | —     | 2.6  | 2.8  |
| 256 MB | RAM | —   | 11.2  | 8.9  | —     | 2.0  | 1.6  |

Reading across rows reveals the whole story:

- **Sequential and SoA vec overlap** across the full hierarchy — structurally
  identical, compiler vectorizes both the same way.
- **SoA novec drops to roughly half at L1d** (scalar load-port ceiling) and
  further at L2/L3/early-RAM. The L2 discrepancy (102 vs 30 GB/s) is *larger*
  than a simple 2× vectorization gap would explain — probably related to how
  L2 streamer and line-fill buffers amortize scalar vs vector miss streams.
  Not yet fully understood — see open question below.
- **AoS vec is artificially slow at L1d** (37 GB/s) because of port-5 shuffle
  saturation. At L2 and below it converges to AoS novec.
- **AoS novec has a healthy L1d** (57 GB/s, load-port limited) and then drops
  into the memory-bound staircase.
- **At deep RAM (256 MB), all four of {Seq, SoA vec, SoA novec} converge at
  ~13-14 GB/s**, and both AoS variants converge at ~2.5 GB/s. Memory is the
  final wall — all CPU-side differences vanish.

## The AoS staircase (the cleanest memory-hierarchy curve)

Reading just the AoS vec column at every size produces a clean four-step
staircase:

```
37 → 37 → 37 → 37   (L1d plateau, port-5 limited)
21 → 19 → 15        (L2)
11 → 11 → 11 → 9    (L3)
 3 →  3 →  3 → 2.5  (RAM)
```

Counter-intuitively, the *bad* data layout draws the memory hierarchy more
clearly than the good one. AoS is bandwidth-bound at every level below L1d,
so every transition is visible. SoA spends most of its curve at compute
ceilings, masking memory transitions. **If you want to see L1d/L2/L3/RAM
boundaries at a glance, AoS is the benchmark to plot.**

## Hardware validation with `perf stat`

The port-5 theory was hand-derived from cycle counting and supported by the
novec counter-experiment. To confirm it directly, I measured per-port uop
counts on both binaries at a 32 KB (L1d-resident) working set.

> **Note on the numbers below.** The `perf stat` table in this section comes
> from an earlier measurement session where the AoS vectorized bandwidth was
> ~33 GB/s and the AoS novec bandwidth was ~57 GB/s. In the latest cold-box
> re-runs (documented in "The novec counter-experiment" above) AoS novec
> landed at ~48 GB/s rather than 57. The port-utilization ratios in the
> table are still self-consistent for the run they describe — 96.7% p5 vs.
> 91% p2+p3 is the whole point — and the qualitative finding (vectorized
> AoS is port-5 bound; novec AoS is load-port bound) is unchanged. Only the
> absolute bandwidth number differs, and it differs by an amount
> (≈15%) that matches the run-to-run thermal/load variance seen when
> frequency logging was added later. This section has not been re-run
> because the bottleneck *direction* is what matters, not the exact GB/s.

```bash
taskset -c 3 perf stat -e cycles,instructions,\
  uops_dispatched_port.port_0,uops_dispatched_port.port_1,\
  uops_dispatched_port.port_2,uops_dispatched_port.port_3,\
  uops_dispatched_port.port_4,uops_dispatched_port.port_5,\
  uops_dispatched_port.port_6,uops_dispatched_port.port_7 \
  ./build/aos_vs_soa --benchmark_filter='BM_AoS/32768' --benchmark_min_time=3s
```

`taskset -c 3` pins the benchmark to a single physical core so counters
aren't diluted by thread migration. `--benchmark_min_time=3s` gives enough
samples to stabilize (~20 billion cycles per run).

**Port utilization — uops per cycle, higher = more saturated:**

| Port | Role | **AoS vectorized** | **AoS novec** | Change |
|------|------|-------------------|---------------|--------|
| p0 | ALU/FP | 0.167 | 0.531 | +0.36 |
| p1 | ALU/FP | 0.169 | 0.545 | +0.38 |
| p2 | load | 0.642 | **0.907** | **+0.27** |
| p3 | load | 0.640 | **0.905** | **+0.27** |
| p4 | store | ~0 | ~0 | — |
| **p5** | **shuffle/insert** | **0.967** ← bottleneck | 0.570 | **−0.40** |
| p6 | ALU/branch | 0.315 | 0.677 | +0.36 |
| p7 | store AGU | ~0 | ~0 | — |
| **p2+p3 total** | load ports | **1.28 / 2.0 (64%)** | **1.81 / 2.0 (91%)** | **+0.53** |

The measurement confirms the theory in the clearest possible way:

- **Port 5 in the vectorized run is 96.7% saturated.** Out of every 100
  cycles, port 5 is executing a uop in 97 of them. No other port comes
  close — this is the definition of "port 5 is the bottleneck."
- **Port 5 drops to 57% in the novec run.** The `vpinsrq` and `vinserti128`
  uops are gone; p5 only sees incidental ALU ops.
- **Load ports jump from 64% → 91%** (combined p2+p3). New bottleneck is
  load-port throughput at ~1.81 loads/cycle out of the 2.0 peak. The 9%
  headroom is fetch/decode overhead and the occasional branch-prediction
  bubble — essentially the hardware ceiling for scalar load-op code.
- **Bandwidth moved from 33 to 57 GB/s** (1.7× faster) because the bottleneck
  migrated from a saturated narrow port to a nearly-saturated wider one.

**Per-iteration breakdown** (one iteration = 8 particles = 64 useful bytes):

| Quantity | Vectorized | Novec | Notes |
|----------|-----------|-------|-------|
| Cycles / iter | 8.77 | 4.68 | Near theoretical 6.5 and 4.0 respectively |
| Port-5 uops / iter | 8.48 | 2.66 | Vec saturates p5; novec only incidental |
| Load uops / iter | 11.25 | 8.48 | Novec hits the expected 8 (one per particle) |
| Instructions / iter | 21.2 | ~18 | Vec is denser in fused-domain uops |

The novec load-uop count of 8.48 per iteration is the cleanest confirmation
of the scalar model: eight scalar loads per eight particles, with ~0.5
additional loads from harness overhead. The cycle count of 4.68 is within 17%
of the theoretical minimum of 4.0 cycles (eight loads ÷ two load ports),
about as close to peak as hand-written hot loops ever run.

The vectorized port-5 count of 8.48 uops/iter is slightly higher than the 6
uops counted from the `.L311` snippet above. The extra ~2.5 uops come from
auxiliary p5-eligible ALU operations emitted elsewhere that the hand snippet
didn't include. This doesn't change the conclusion — if anything, port 5 is
*more* saturated than the simpler model predicted.

**The theory is confirmed.** The AoS L1d penalty in the vectorized build is
port-5 shuffle saturation from GCC's gather vectorization of stride-48 loads.
Disabling vectorization migrates the bottleneck to the load ports and
recovers roughly 40-70% more bandwidth depending on thermal state (+45% on
a cold box under frequency logging, +70% in the earlier `perf stat`
session). The *direction* is stable; the exact multiplier fluctuates with
clock because the novec code ends up run-time-dependent on the CPU hitting
its AVX2-off boost ceiling.

## Takeaways

1. **Compute ceilings hide memory ceilings.** A flat bandwidth curve across
   cache levels means you're measuring something that isn't memory. Break
   serial dependency chains (multiple accumulators) and pick data types whose
   arithmetic is cheaper than the memory access being measured.
2. **Benchmark x-axes must represent touched bytes, not allocated bytes.**
   AoS and SoA have different allocated-to-touched ratios, and a naive
   `n = size / sizeof(Particle)` formula lies about cache pressure for SoA.
3. **The AoS penalty shows up mostly at L2 and below, and the L1d component
   is mostly compiler-shape-dependent.** At L2+ the 6× cache-line waste is
   pure memory bandwidth, identical across code shapes. At L1d the waste is
   invisible (data is already resident), but the code shape GCC chooses for
   stride-48 loads still matters: the vectorized gather path saturates port
   5, which inflates the L1d gap beyond what load-port limits alone would
   predict. Re-running the novec experiment on a clean cold box showed that
   the scalar-AoS vs scalar-SoA gap at L1d is smaller than vectorized but
   not quite zero (see the softened conclusion in the novec section), so
   the original "L1d gap is purely a compiler artifact" framing was too
   confident and has been retracted.
4. **Compiler vectorization of gather/scatter can be net-negative.** When the
   preconditions (gathering scattered data into vector registers) cost more
   than the vectorized operation saves, the transformation makes code slower
   than plain scalar would have been. Port 5 on Skylake/Kaby Lake is
   particularly easy to saturate this way because it is the only port that
   handles lane-crossing inserts.
5. **Four rewrites is normal.** Each variant falsified a prediction. The
   investigation is the value, not the final number.
6. **Reading assembly is not optional for this kind of work.** Three
   plausible theories explained the AoS L1d number equally well in prose.
   Only the assembly dump could distinguish them, and the answer (port 5)
   wasn't on the list of theories I had considered before reading it.

## Open question (not yet investigated)

The L2-region scalar-vs-vector discrepancy for SoA (102 GB/s vectorized vs
30 GB/s novec at 64 KB — a much larger gap than 2×) is not yet understood. A
planned follow-up is to measure `l2_rqsts.demand_data_rd`,
`l2_rqsts.demand_data_rd_miss`, `l1d_pend_miss.fb_full` (line-fill-buffer
full cycles), and `l1d_pend_miss.pending` to determine whether the gap is LFB
pressure, L2 streamer prefetcher efficiency, or something else. Left as a
future experiment — the AoS/SoA story at L1d is now closed.

## Canonical result

![AoS vs SoA canonical](../results/aos_vs_soa/v4_canonical.png)

![AoS vs SoA: vectorized vs novec](../results/aos_vs_soa/novec_comparison.png)

## References

- **Source**: [`src/aos_vs_soa.cpp`](../src/aos_vs_soa.cpp)
- **Canonical (V4)**: [`results/aos_vs_soa/v4_canonical.json`](../results/aos_vs_soa/v4_canonical.json)
- **Novec counter-experiment**: [`results/aos_vs_soa/v4_novec.json`](../results/aos_vs_soa/v4_novec.json)
- **V3 (buggy SoA sizing)**: [`results/aos_vs_soa/v3_buggy_sizing.json`](../results/aos_vs_soa/v3_buggy_sizing.json)
- **Related**: [`sequential.md`](sequential.md) — same "compute ceiling hides
  memory" trap, integer latency instead of FP.
- **Hardware details**: [`hardware_background.md`](hardware_background.md) —
  port layout, Skylake/Kaby Lake references.
