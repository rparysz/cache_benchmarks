# Write Bandwidth — Notes

The suite's first **write-bound** benchmark. Every other test is read-bound
(accumulators live in registers; the hot loop only loads). This one streams
stores across an array and measures **write bandwidth (GB/s)**, exposing an
asymmetry the read benchmarks miss: on write-allocate caches, a plain store
**secretly reads** the line it is about to overwrite.

Two variants:

- **Plain store** — `data[i] = 0x42`, an ordinary store.
- **Non-temporal store** — `_mm_stream_si64` (emits `movnti`), which bypasses the
  cache through write-combining buffers.

All runs use `-O2 -march=native` and are pinned to a single core with
`taskset -c 3`. Working set = the array size; the sweep 4 KB → 256 MB crosses
L1 → L2 → L3 → RAM, same as the read benchmarks.

## Canonical result

| Region | Working set | Plain (GB/s) | NT (GB/s) | NT / Plain |
|--------|-------------|--------------|-----------|------------|
| L1d | 4 - 32 KB    | ~26      | ~18 | 0.7× |
| L2  | 64 - 256 KB  | ~23      | ~14 | 0.6× |
| L3  | 512 KB - 4 MB| ~18-21   | ~14 | 0.7× |
| RAM | 8 - 256 MB   | ~5.9     | ~14 | ~2.4× |

![Write bandwidth — plain vs non-temporal store](../results/write_bandwidth/canonical.png)

Two curves with opposite shapes. **Plain store** is a cache staircase like the
read benchmarks (~26 GB/s in L1 down to ~6 in RAM) — it wins wherever the data
fits cache, then **falls off a cliff past L3**. **NT store** is a flat line at
~14 GB/s *everywhere* — it loses badly in cache but holds steady and **wins ~2.4×
once the data spills to RAM**. They cross right at the L3 boundary (6 MB): plain
leads at 4 MB, NT leads by 8 MB.

## Variant 1: plain store — write-allocate and RFO

```cpp
for (i) data[i] = 0x42;
```

**In cache** (≤ L3): after the first pass the lines are resident and **owned
(Modified state)**, so every later iteration is a **write hit** — no miss, no
extra traffic, just cache-write-port bandwidth (~21-26 GB/s). This is the best
case for writes.

**Out of cache** (> L3): each pass evicts the previous, so every line is a
**write miss → Read-For-Ownership (RFO)**. Because caches move whole 64-byte
lines (write-allocate + write-back), the CPU must first **read the line from
RAM** — to preserve the bytes it is not overwriting and to acquire exclusive
ownership — then modify it, then **write the dirty line back** on eviction. Two
bus trips per line for a write-only job, so the useful write bandwidth collapses
to ~6 GB/s. The reported GB/s counts only bytes *written*, so the RFO read is
uncounted overhead — it shows up as the number being roughly half of what the
memory bus is actually doing.

**Assembly.** `mov QWORD PTR [rax], 0x42` — a scalar 8-byte immediate store,
unrolled ×2. No `memset`/`rep stos` (verified in `objdump`), so it is a genuine
plain store that pays the RFO. It is *scalar* (not vectorized), so the in-cache
numbers are store-port-limited rather than peak; the out-of-cache regime — the
one we care about — is memory-bound regardless of store width.

## Variant 2: non-temporal store — `movnti` and write-combining

```cpp
for (i) _mm_stream_si64(reinterpret_cast<long long*>(&data[i]), value);
_mm_sfence();
```

`_mm_stream_si64` emits `movnti`, a **non-temporal** store that routes through
small **write-combining (WC) buffers** straight toward memory: it accumulates a
full 64-byte line, flushes it in one burst, and **never reads the old line** — no
RFO, no cache pollution. `_mm_sfence()` commits the weakly-ordered NT stores
before the timed iteration ends. Unlike the plain store, the compiler **cannot**
fold this into `memset`, so the `movnti` path is guaranteed.

**Assembly.** `mov edx, 0x42` (hoisted out of the loop — writing `edx`
zero-extends into `rdx`), then `movnti QWORD PTR [rax], rdx` per element. The
extra register load exists because `movnti` has **no immediate form** — it stores
from a register — unlike the plain `mov` which takes the immediate directly.

**Result:** flat ~14 GB/s across *every* size — **no cache staircase**, because it
never touches the cache. That flatness is the signature of a cache-oblivious
stream. Notably ~14 GB/s ≈ the sequential-*read* RAM bandwidth (~13), which makes
sense: a write-only stream and a read-only stream both move one line per element.

## Why RFO is a read *and* a write

Caches transfer **64-byte lines**, not bytes, and x86 is **write-allocate +
write-back**. A store to an uncached line therefore:

1. **reads** the whole 64-byte line from RAM — to keep the bytes it is *not*
   overwriting, and to take exclusive ownership (MESI M/E);
2. **modifies** the target bytes, marking the line dirty;
3. **writes** the dirty line back to RAM on eviction.

Even when the program overwrites the *entire* line (as here), the plain store
still RFOs, because the cache acts on the **first store to the line** — before it
can know the rest of the line will also be written. NT stores skip step 1
entirely, which is the whole ~2.4× difference in the RAM region.

## Takeaways

- **Writes are asymmetric.** A plain store to uncached memory pays an RFO read —
  read + write traffic for a write-only job. That is invisible in the read
  benchmarks and is the reason this one exists.
- **NT stores skip the RFO** (write-combining, cache-bypassing) → ~2.4× past L3 —
  but they **lose in cache**, because they throw away the cache benefit a plain
  store enjoys. Use them only for large, write-once, no-reuse streams.
- **NT has no cache staircase.** Its flat curve is the tell that it ignores the
  cache hierarchy entirely.
- **The metric counts useful bytes only**, so the plain store's RFO reads surface
  as depressed bandwidth rather than extra traffic — the asymmetry made visible.
- **Rule of thumb:** the crossover sits at last-level cache — NT stores help only
  when the working set won't fit L3.

## References

- **Source:** [`src/write_bandwidth.cpp`](../src/write_bandwidth.cpp)
- **Results:** [`results/write_bandwidth/canonical.json`](../results/write_bandwidth/canonical.json)
- **Plot:** [`results/write_bandwidth/canonical.png`](../results/write_bandwidth/canonical.png)
  (generated by [`scripts/plot_results.py`](../scripts/plot_results.py))
- **Related:** [`sequential.md`](sequential.md) (read bandwidth — NT write ≈ read
  at RAM), [`false_sharing.md`](false_sharing.md) (MESI ownership / the store side
  of coherence).
