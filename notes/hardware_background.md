# Hardware Background

Reference notes for the test machine and microarchitecture. Useful context
while reading the benchmark deep-dives.

## Test CPU: Intel Core i7-8550U

| Property | Value |
|----------|-------|
| Marketing name | Core i7-8550U (Kaby Lake R) |
| Core microarchitecture | **Skylake** (identical core, different uncore/process) |
| Cores / threads | 4 / 8 |
| Base / boost clock | 1.8 GHz / 4.0 GHz |
| Issue width | 4 uops/cycle (fused domain) |
| Retirement width | 4 uops/cycle |
| ROB size | 224 entries |

**Kaby Lake = Skylake core.** Intel rebranded the same core microarchitecture
across Skylake (6th gen), Kaby Lake (7th gen), and Coffee Lake (8th gen). The
core pipeline, execution ports, cache hierarchy, and prefetchers are identical.
Any Skylake optimization manual, port table, or latency reference applies
directly to this CPU.

## Cache hierarchy

| Level | Size | Associativity | Line size | Latency | Bandwidth (measured) |
|-------|------|---------------|-----------|---------|---------------------|
| L1d | 32 KB per core | 8-way | 64 B | ~4 cycles | ~100 GB/s (8-acc sequential), ~133 GB/s (16-acc ceiling) |
| L1i | 32 KB per core | 8-way | 64 B | — | — |
| L2 | 256 KB per core | 4-way | 64 B | ~12 cycles | ~86-93 GB/s (sequential) |
| L3 | 6 MB shared | 12-way | 64 B | ~30-40 cycles | ~30-50 GB/s (sequential) |
| RAM | DDR4-2400, single channel | — | — | ~90 ns (measured via pointer chasing) | ~11-16 GB/s (sequential) |

L1d and L2 are per-core and private. L3 is shared across all cores (inclusive
on Skylake client). Latencies are approximate — load-to-use latency depends on
addressing mode, alignment, and whether the access hits a fill buffer.

The RAM figures are **single-channel**: every benchmark here was captured with a
single 8 GB DIMM (the second slot empty), so RAM bandwidth is roughly half what
this DDR4-2400 controller can do in dual-channel. Latency (~90 ns) is unaffected
by channel count — dual-channel adds bandwidth, not lower latency.

## Execution ports (Skylake/Kaby Lake)

| Port | Primary role | Key instructions |
|------|-------------|-----------------|
| p0 | ALU, FP mul/div, FMA | `vmulpd`, `vfmadd*`, `vdivpd` |
| p1 | ALU, FP add, integer mul | `vaddpd`, `vpaddq`, `imul` |
| p2 | Load (+ AGU) | All loads |
| p3 | Load (+ AGU) | All loads |
| p4 | Store data | Store data path |
| p5 | ALU, shuffles, inserts | `vpinsrq`, `vinserti128`, `vperm*`, `vpshufd` |
| p6 | ALU, branch | `jne`, `jcc`, `add` (when other ports busy) |
| p7 | Store AGU | Store address generation |

**Key bottleneck ports encountered in these benchmarks:**

- **Port 5** — the only port that handles lane-crossing shuffles and
  scalar-to-vector inserts (`vpinsrq`, `vinserti128`). When GCC
  gather-vectorizes AoS stride-48 loads, port 5 saturates at 96.7% and
  becomes the throughput limiter. See [aos_vs_soa.md](aos_vs_soa.md).
- **Ports 2+3** — the two load ports. Peak throughput is 2 loads/cycle. In
  scalar code this gives a ceiling around 50 GB/s at L1d for 8-byte loads
  (measured ~48 GB/s for AoS novec). In vectorized code, a single
  `vmovdqu ymm, [mem]` consumes one load port but delivers 32 bytes,
  pushing the ceiling much higher — the 8-accumulator sequential benchmark
  hits ~100 GB/s, the 16-accumulator variant ~133 GB/s. See
  [sequential.md](sequential.md) and [aos_vs_soa.md](aos_vs_soa.md).

## Hardware prefetchers

Skylake client has four hardware prefetchers that can be individually
enabled/disabled via MSRs:

| Prefetcher | Scope | What it detects | Notes |
|------------|-------|----------------|-------|
| L1 DCU (Data Cache Unit) | L1d | Sequential streams, next-line | Very short lookahead |
| L1 IP (Instruction Pointer) | L1d | Per-IP stride patterns | Tracks stride per load instruction |
| L2 streamer | L2 | Sequential streams, up to 2 per core | Longest lookahead, most aggressive |
| L2 adjacent line | L2 | Fetches the pair line (128 B aligned) | Sometimes called "spatial prefetcher" |

**Observations from these benchmarks:**

- **Sequential access**: prefetchers keep RAM bandwidth at ~11-16 GB/s.
  Without prefetching (random pointer chasing), the same bandwidth drops
  below 1 GB/s. The prefetcher is the difference between usable and unusable
  RAM throughput for streaming workloads.
- **Pointer chasing (sequential)**: L1 IP prefetcher detects the constant
  stride and prefetches ahead, yielding ~6 ns/chase at RAM sizes vs ~90 ns
  random. Effectiveness decreases as node size grows (NPAD=31 → 4 lines per
  node, prefetcher can't keep up). See [pointer_chasing.md](pointer_chasing.md).
- **Stride 128**: slightly slower than stride 64 at L1d. The L2 adjacent-line
  prefetcher fetches 128-byte aligned pairs, which helps stride-64 more than
  stride-128 (every other fetch is wasted for stride 128). See
  [strided.md](strided.md).

## Key latencies to remember

| Operation | Latency | Source |
|-----------|---------|--------|
| L1d load-to-use | ~4 cycles | Intel optimization manual |
| L2 load-to-use | ~12 cycles | Intel optimization manual |
| L3 load-to-use | ~30-40 cycles | Intel optimization manual / measured |
| DRAM round-trip | ~90 ns (~360 cycles at 4 GHz) | Measured (pointer chasing NPAD=7 random) |
| `vaddpd ymm` (FP add) | 4 cycles latency, 1/cycle throughput (p0/p1) | Agner Fog |
| `vpaddq ymm` (int add) | 1 cycle latency, 1/cycle throughput (p0/p1/p5) | Agner Fog |
| `vpinsrq` | 2 uops (p5 + load port), 1/cycle throughput (p5-limited) | uops.info |
| `vinserti128` | 1 uop (p5), 1/cycle throughput | uops.info |

## Reading list

These are the references that were most useful during this project:

| Resource | What it covers | URL |
|----------|---------------|-----|
| Ulrich Drepper, *What Every Programmer Should Know About Memory* (2007) | The paper this project follows. Cache hierarchy, prefetchers, TLB, NUMA. Still accurate for the fundamentals. | lwn.net/Articles/250967 |
| Agner Fog, *Instruction Tables* | Per-instruction latency and throughput for every x86 microarchitecture. The definitive reference for cycle counting. | agner.org/optimize |
| Agner Fog, *Microarchitecture of Intel, AMD, and VIA CPUs* | Pipeline details, port assignments, out-of-order engine mechanics. | agner.org/optimize |
| Intel 64 and IA-32 Architectures Optimization Reference Manual | Official source for cache sizes, prefetcher descriptions, and microarchitecture diagrams. | intel.com (search "optimization reference manual") |
| uops.info | Measured port mappings and latencies for every instruction on every microarchitecture. More reliable than Intel's manual for port assignments. | uops.info |
| WikiChip — Skylake (client) | Block diagrams, die shots, cache hierarchy details. Good visual overview. | en.wikichip.org/wiki/intel/microarchitectures/skylake_(client) |
| 7-cpu.com — Skylake | Measured memory latencies at every cache level. Useful for cross-checking your own numbers. | 7-cpu.com/cpu/Skylake.html |
| Travis Downs' blog | Deep dives on memory-level parallelism, LFBs, store forwarding. Advanced but invaluable for the open questions in these benchmarks. | travisdowns.github.io |

## Books

| Book | Relevance |
|------|-----------|
| *Computer Architecture: A Quantitative Approach* (Hennessy & Patterson) | Memory hierarchy fundamentals, bandwidth/latency modeling, Amdahl's law |
| *Computer Systems: A Programmer's Perspective* (Bryant & O'Hallaron) | Cache geometry, virtual memory, linking — the programmer-facing view |
| *Is Parallel Programming Hard, And, If So, What Can You Do About It?* (McKenney) | Memory ordering, barriers, cache coherence — relevant when these benchmarks expand to multi-core |
