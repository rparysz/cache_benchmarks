# Pointer Chasing — Notes

## What the benchmark does

Builds a circular linked list of nodes and traverses it by following pointers
from one node to the next, measuring **latency** (nanoseconds per dereference)
rather than bandwidth. Each load depends on the result of the previous load —
the CPU cannot begin the next fetch until it has decoded the current node's
pointer. This is the pattern that defeats prefetchers and parallelism alike.

This benchmark is Drepper's Section 3.2 experiment, replicated on Kaby Lake.
It is the most important benchmark in the suite for understanding the
*latency* side of the memory hierarchy, as opposed to the bandwidth side that
sequential and strided measure.

All runs use `-O2 -march=native` and are pinned to a single core with
`taskset -c 3`.

## Two access patterns, four node sizes

Two list-building strategies are tested:

- **Sequential**: nodes are allocated in memory order and linked in the same
  order (`node[i].next = &node[i+1]`). The stream of accessed addresses is
  predictable, and the hardware prefetcher can detect the pattern.
- **Random**: the same nodes are shuffled into a random permutation and
  linked in that order. Each chase jumps to an unpredictable address, which
  defeats the prefetcher.

Four node sizes via an `NPAD` template parameter:

- **NPAD=0** (8 B/node): 8 nodes share one cache line.
- **NPAD=7** (64 B/node): exactly one node per cache line.
- **NPAD=15** (128 B/node): one node per 2 cache lines.
- **NPAD=31** (256 B/node): one node per 4 cache lines.

The NPAD parameter lets us vary spatial locality *within* a pointer-chase
pattern. At NPAD=0, 7 out of every 8 chases are to nodes that share a cache
line with the previous chase (already in L1d after the miss that loaded the
line), so even a "random" traversal is not truly random at the cache-line
level. At NPAD=7, every chase starts from a fresh cache line, so the
prefetcher's job is to predict which *line* comes next. At NPAD=15 and 31,
even if the prefetcher predicts the stream correctly, each node requires
multiple cache lines to be in place, and the prefetcher's throughput
becomes the limit.

## Index calculation subtlety

A small note on the initialization loop. The nodes are built in an array,
and each `node[i].next` is set to the next node by index. The circular-list
wiring uses the formula:

```cpp
nodes[i].next = &nodes[(i + 1) % nodes.size()];
```

The `% nodes.size()` is there only to wrap the *last* node back to the
first. For every other node (`i < size - 1`), it's equivalent to `i + 1`.
A simpler rewrite would be an `if` that handles the wraparound explicitly:

```cpp
nodes[i].next = &nodes[i + 1 == nodes.size() ? 0 : i + 1];
```

Either is fine. The modulo form is a single expression, the conditional form
avoids a (cheap) division. Since this loop runs once at setup and the
benchmark timing only covers the traversal, the difference is invisible in
the results.

## Canonical results

All values are **nanoseconds per dereference** at a 256 MB working set
(deep RAM, where the prefetcher has to do real work). Lower is better.

| NPAD | Sequential | Random | Prefetcher speedup |
|------|-----------|--------|--------------------|
| 0 (8 B)   | ~1.4 ns  | ~91 ns | 65× (but see note) |
| 7 (64 B)  | ~6.0 ns  | ~94 ns | **16× — pure prefetcher** |
| 15 (128 B) | ~13.7 ns | ~91 ns | 6.6× |
| 31 (256 B) | ~24.5 ns | ~89 ns | 3.6× |

![Pointer chasing — NPAD sweep, sequential vs random](../results/pointer_chasing/canonical.png)

### Reading the table row by row

**NPAD=0 (65×)**: the speedup looks huge, but it's misleading. When 8 nodes
share a cache line, 7 out of every 8 "chases" land on a node whose cache
line is already in L1d from the previous chase. The 1.4 ns sequential
latency is a weighted average: 7/8 cache hits at ~1 cycle (0.25 ns) plus
1/8 cache misses at the prefetcher-assisted rate. The huge 65× speedup is
**compound**: pure prefetcher + same-line hit rate, not purely prefetcher
benefit. For a clean measure of the prefetcher's effect, NPAD=7 is the
right row to look at.

**NPAD=7 (16×)**: this is the clean measurement. One node per cache line, so
every chase requires a new cache line fetch. Sequential: 6 ns (the
prefetcher has brought the line into L1d before the program needs it).
Random: 94 ns (true RAM latency). The 16× speedup is **pure prefetcher
effect** — the hardware predicts the access pattern and hides most of the
~90 ns DRAM round-trip behind computation.

**NPAD=15 (6.6×)**: now each node spans 2 cache lines, so the prefetcher has
to deliver 2 lines per dereference. It can no longer fully hide the latency
because its throughput is the limit, not its prediction. Sequential latency
climbs to ~14 ns.

**NPAD=31 (3.6×)**: each node spans 4 cache lines, and the prefetcher
struggles even harder. Sequential latency reaches ~24 ns. The speedup
continues to shrink because the prefetcher is saturated.

### The 90 ns observation

Look at the **Random** column: ~89-94 ns across all four NPAD values. This is
the **true DRAM round-trip latency** on this hardware. It does not depend on
node size because the bottleneck is not data transfer (which would scale with
bytes) but DRAM access time (which is fixed per miss).

This number is surprisingly important. Every other "RAM region" measurement
in this benchmark suite is secretly limited by this 90 ns wall, divided by
how much parallelism the code can expose:

- **Sequential bandwidth at 256 MB** (~14 GB/s) = cache line (64 B) / effective
  per-line latency. The prefetcher keeps many requests in flight, so the
  effective latency is much less than 90 ns — roughly 64 B / 14 GB/s ≈ 4.6 ns
  per line. That means the prefetcher + memory-level-parallelism machinery
  is hiding the 90 ns latency about 20× behind concurrent requests.
- **Strided access at 256 MB** (~0.2 GB/s for 1-byte-per-line reads) = 1 byte
  per ~5 ns. That's worse than sequential per byte, but the prefetcher still
  helps because the stride is predictable.
- **Random pointer chase at 256 MB** (~90 ns) = no prefetcher, no parallelism,
  full serial latency. This is the worst case — the hardware floor.

**At L1d sizes (≤32 KB), sequential and random latencies converge at
~1 ns** because the entire working set fits in L1d and access order is
irrelevant. Every load is a hit no matter what order you touch the nodes in.
That crossing point (the latency curves coming apart as working set exceeds
L1d) is one of the most visible places to see cache boundaries in
measurements.

## Bandwidth measurement — why I don't report it here

An earlier version of this benchmark reported bandwidth in GB/s. That is
misleading for pointer chasing because the workload is inherently serial:
each `node = node->next` depends on the previous dereference, so bandwidth
is `cache_line_size / latency_per_chase`. For NPAD=7 sequential at 256 MB,
that's 64 / 6.0 = 10.7 GB/s — which is a real number, but it doesn't
convey the latency-limited nature of the work.

The latency measurement is the honest story: "this code takes 6 ns per
node on sequential random-access patterns, and 94 ns per node on random
patterns." Bandwidth would hide the fact that each chase is serial and that
no amount of parallelism can help unless you change the data structure.

## References

- **Source**: [`src/pointer_chasing.cpp`](../src/pointer_chasing.cpp)
- **Canonical result**: [`results/pointer_chasing/canonical.json`](../results/pointer_chasing/canonical.json)
- **Plot**: [`results/pointer_chasing/canonical.png`](../results/pointer_chasing/canonical.png)
- **Related**: Drepper, *What Every Programmer Should Know About Memory*,
  Section 3.2, Figures 3.10–3.15.
