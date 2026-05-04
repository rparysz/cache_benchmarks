# False Sharing — Notes

## What the benchmark does

Multiple threads each increment their own counter in a tight loop. The only
thing that changes between variants is *where the counters live in memory*:
packed onto a single cache line, or spread across separate lines. The
per-increment cost tells you how much the hardware pays when unrelated data
happens to share a line.

This is the classic false-sharing demonstration from Drepper §6.4.3,
replicated on Kaby Lake. It is also the first benchmark in this suite that
involves more than one thread — everything before was single-core bandwidth,
ILP, or layout. Here the story is cache coherence: MESI, the store buffer,
and memory-ordering machine clears.

## Four variants

| Variant | Storage | What it measures |
|---------|---------|------------------|
| `BM_ThreadLocal` | register-resident local, published once after the loop | Absolute ceiling. No memory traffic in the hot loop. |
| `BM_RawPadded` | 8 counters, each `alignas(64)` on its own cache line | The fix for false sharing. Each thread owns its line. |
| `BM_RawShared` | 8 plain `uint64_t` packed onto one 64-byte line | Classic false-sharing scenario, but *without* the `lock` prefix. Technically a C++ data race; used to isolate pure cache-coherence cost. |
| `BM_AtomicShared` | 8 `std::atomic<uint64_t>` on one line, `fetch_add(1, relaxed)` | Shared line + serializing atomic. Separates "MESI cost" from "atomic-instruction cost." |

Each variant registers `->ThreadRange(1, 8)->UseRealTime()`. Google
Benchmark's `ThreadRange(min, max)` sweeps **powers of 2** (inclusive on both
ends), so `(1, 8)` expands to `{1, 2, 4, 8}` — four thread counts. 
Four counts × four variants = 16 data points. Metric is
`SetItemsProcessed(state.iterations())`, which Google Benchmark reports as
`items_per_second`; the plot script renders it as nanoseconds per increment.

## Canonical result

Invocation: `taskset -c 0-7 ./build/false_sharing --benchmark_min_time=1.0s`.
Thread placement is left to the kernel (no per-thread
`pthread_setaffinity_np`) — for N ≤ 8 it spreads threads across N logical
CPUs.

![False sharing — ns per increment vs thread count](../results/false_sharing/canonical.png)

| Threads | ThreadLocal | RawPadded | RawShared | AtomicShared |
|---------|-------------|-----------|-----------|--------------|
| 1       | 0.29 ns     | 1.79 ns   | 1.79 ns   | 5.54 ns      |
| 2       | 0.30 ns     | 1.75 ns   | 2.19 ns   | 36.0 ns      |
| 4       | 0.29 ns     | 1.74 ns   | 2.34 ns   | 78.6 ns      |
| 8       | 0.58 ns     | 1.81 ns   | 8.50 ns   | 153 ns       |

Key ratios at 4 threads:

- AtomicShared / RawPadded ≈ **45×**
- AtomicShared / RawShared ≈ **33×** — same shared line, same threads;
  the only thing that changed is the `lock` prefix on the increment
  (`lock incq` instead of plain `incq`). That one byte forces atomicity,
  drains the store buffer, and transfers cache-line ownership on every
  op — so the 33× is the cost of those guarantees on a contested line.
- RawShared / RawPadded ≈ **1.35×** — much smaller than I expected

That last ratio is the interesting finding. See "The RawShared surprise"
below.

## Assembly — what the inner loop actually runs

All four variants produce a very different hot loop. The key lines (obtained
with `objdump -d --disassemble='_ZL12BM_RawShared*' build/false_sharing` and
friends; everything important fits below):

**`BM_RawShared`** — load, increment in register, store back.

```asm
mov    0x405380(,%rcx,8),%rax   # load  slot[idx] → rax
inc    %rax                     # rax++
mov    %rax,0x405380(,%rcx,8)   # store rax → slot[idx]
```

Three instructions, no `lock` prefix. The store goes into the core's store
buffer and drains lazily into L1d.

**`BM_RawPadded`** — identical shape. Only the address pattern differs
(separate line per thread).

```asm
mov    0x405180(%rcx),%rax
inc    %rax
mov    %rax,0x405180(%rcx)
```

**`BM_AtomicShared`** — single memory-destination atomic RMW.

```asm
lock incq 0x405140(,%rcx,8)
```

One instruction, but with the `lock` prefix: the CPU obtains exclusive
ownership of the cache line, flushes the store buffer, performs the
increment, then makes the result globally visible before retiring. No
speculation past it.

**`BM_ThreadLocal`** — no memory operand at all.

```asm
inc    %rax                     # register-resident counter
dec    %rdx                     # iteration count
jne    .loop
```

Two ALU ops per iteration. At ~1 cycle each and Skylake's 4-wide retirement,
this saturates at 0.29 ns per increment on a 3.5 GHz core.

## The RawShared surprise

My prior expectation (and Drepper's framing) was that packing N plain
counters onto one cache line would cause the line to ping-pong between cores
on every write, costing tens of nanoseconds per increment. What I actually
measure is only ~2.3 ns at 4 threads — barely slower than the single-thread
baseline.

`perf stat` revealed why. Running for ~1 second at 4 threads:

| Counter | RawShared/1t | RawShared/4t | RawPadded/4t | AtomicShared/4t |
|---------|--------------|--------------|--------------|-----------------|
| cycles | 17.0 B | 14.0 B | 12.8 B | 11.4 B |
| instructions | 16.0 B | 9.8 B | 12.3 B | 107 M |
| `mem_inst_retired.lock_loads` | 165 | 1,497 | 1,050 | **39.5 M** |
| `mem_load_l3_hit_retired.xsnp_hitm` | 1 | 217 K | 504 | **37.7 M** |
| `machine_clears.memory_ordering` | 2 K | **41 M** | 28 K | 61 |

`xsnp_hitm` fires when a load retires on a line that was dirty in *another*
core's cache — in other words, once per inter-core line transfer. For
`AtomicShared/4t` this number (37.7 M) matches the instruction count (107 M,
~1/3 of which are the timed `lock incq` ops): every atomic op genuinely
ping-pongs the line. Classical MESI, textbook behaviour.

`RawShared/4t` only sees 217 K `xsnp_hitm` events — **170× fewer line
transfers** than `AtomicShared` at the same thread count. The x86 TSO memory
model lets each core hold the line in the Modified state locally, commit
many stores into its store buffer, and only surrender the line when another
core issues a load (or when the store buffer drains). Over a window of, say,
10 000 iterations per core, the line might transfer ownership only a
handful of times. MESI does not cost what the textbook says when the stores
are plain and there is no fence.

Where does the extra ~0.6 ns then come from? `machine_clears.memory_ordering`
is the smoking gun: 41 M clears, roughly **one clear per 29 cycles of work**.
Each clear is a speculative-pipeline flush triggered when a cross-core snoop
invalidates a load the CPU already executed out of order. The core
re-issues the load from the freshly updated line, pays ~10–20 cycles, and
continues. 41 M × ~15 cycles at 3.5 GHz ≈ 0.6 G cycles lost, spread over
~1.2 G iterations per thread — about **~0.5 ns of extra latency per
increment**. That is exactly the gap between RawPadded (1.74 ns) and
RawShared (2.34 ns).

The real cost of false sharing on x86 under TSO, when the operations are
plain writes, is **memory-ordering machine clears, not cache-line
transfers**. Atomics remove the ambiguity by forcing both. Until then,
store-buffer batching hides most of the MESI traffic.

The 8-thread jump (2.34 → 8.50 ns) is a separate effect. With 8 threads on
4 physical cores, each physical core hosts two SMT (Simultaneous Multi-Threading) 
siblings. The siblings share one L1d (no MESI between them) but compete for the 
store buffer, the load/store ports, and the memory-ordering logic. More speculative 
loads are in flight at any moment on any core, so any cross-core snoop flushes more
work.

## The AtomicShared staircase

| Threads | ns / op | Inter-core line transfers (xsnp_hitm/s) |
|---------|---------|-----------------------------------------|
| 1 | 5.54 ns | ~0 (no other core to transfer to) |
| 2 | 36.0 ns | ~15 M/s per thread |
| 4 | 78.6 ns | ~12.6 M/s per thread |
| 8 | 153 ns  | ~5 M/s per thread |

This is the staircase the textbook promises. A `lock xadd` (or `lock incq`)
forces:

1. Move the cache line to the Modified state in the issuing core.
2. Drain the store buffer before the `lock` retires.
3. Invalidate the line in every other core's L1d.
4. Make the result globally visible before the next instruction.

There is no batching: every atomic increment is a full RFO + invalidation
round trip. At 4 threads, the critical path becomes the inter-core transfer
latency — roughly one transfer per core per ~300 cycles. On Kaby Lake, L3
hit latency is ~35 cycles, but a line bouncing through multiple cores has
to go through snoop filters and possibly the L3 cache agent, bloating the
effective latency to ~250+ cycles.

## Takeaways

1. **Padding per-thread data to 64 bytes is cheap and eliminates false
   sharing**. `alignas(64)` is the one-line fix. In real code this matters
   for per-thread counters, per-thread allocator slabs, and per-core
   book-keeping fields in lock-free queues.
2. **The textbook "MESI ping-pong" cost only fully materializes under
   synchronized operations** (`lock`-prefixed RMW or explicit fences).
   Plain stores benefit from TSO's store-buffer batching.
3. **Memory-ordering machine clears are the hidden cost on x86**. They
   show up in `perf stat -e machine_clears.memory_ordering` and
   `tma_machine_clears` under the top-down breakdown. A well-tuned
   single-thread program can see close to zero; a false-sharing bug
   pushes it into the tens of millions per second.
4. **SMT doubles the impact of any coherence issue**. Two siblings on
   one physical core share L1d (so no MESI between them), but they also
   share the speculative-load machinery, so any inter-core snoop flushes
   twice as much in-flight work.
5. **Atomics are not a drop-in replacement for "just put it on its own
   line"**. The lock prefix brings 20–50 ns of serialization at modest
   contention. If the access pattern is uncontended in practice, a
   plain-data + padding design is dramatically faster than an
   atomic + shared-line design.

## Files

- Benchmark source: [`../src/false_sharing.cpp`](../src/false_sharing.cpp)
- Canonical JSON: [`../results/false_sharing/canonical.json`](../results/false_sharing/canonical.json)
- Plot (log y): [`../results/false_sharing/canonical.png`](../results/false_sharing/canonical.png)
- Plot (linear y): [`../results/false_sharing/canonical_linear.png`](../results/false_sharing/canonical_linear.png)
- `perf stat` captures: [`../perf/false_sharing/`](../perf/false_sharing/) — `raw_shared_1t.txt`, `raw_shared_4t.txt`, `raw_padded_4t.txt`, `atomic_shared_4t.txt`
