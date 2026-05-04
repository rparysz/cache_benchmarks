#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// False-sharing benchmark — replicates Drepper §6.4.3.
//
// Four variants, each with ThreadRange(1, 8). Every thread picks its own slot
// via state.thread_index() and increments it in a tight loop. What differs is
// where the slot lives:
//
//   RawShared    : 8 plain uint64_t packed into one 64-byte line.
//                  All threads hit the same cache line → MESI ping-pong.
//                  Data race under C++ rules, but that is the point.
//   RawPadded    : 8 uint64_t on separate 64-byte lines.
//                  Threads touch disjoint lines → no coherence traffic.
//                  Shows the fix: same code, aligned properly.
//   AtomicShared : 8 std::atomic<uint64_t> packed into one 64-byte line.
//                  Every increment is `lock xadd` — serialization on top of
//                  the MESI ping-pong. The gap between this and RawShared is
//                  the lock-prefix tax alone.
//   ThreadLocal  : each thread increments a register-resident local and
//                  publishes once after the loop. Zero coherence traffic in
//                  the hot loop → upper-bound ceiling.
//
// Metric: SetItemsProcessed(iterations) → Google Benchmark reports ns/item,
// which reads directly as "nanoseconds per increment."
// ─────────────────────────────────────────────────────────────────────────────

// Eight plain counters packed into a single 64-byte cache line. All threads
// hit this one line regardless of which slot they own. alignas(64) guarantees
// the struct sits at a line boundary — otherwise the last slot could spill
// onto a second line and weaken the demonstration.
struct alignas(64) SharedLine
{
  uint64_t counters[8];
};
static_assert(sizeof(SharedLine) == 64);

// One counter per 64-byte line. alignas(64) guarantees each instance starts
// on a line boundary; the pad[] fills the rest so consecutive PaddedCounters
// in an array land on disjoint lines.
struct alignas(64) PaddedCounter
{
  uint64_t value;
  char pad[64 - sizeof(uint64_t)];
};
static_assert(sizeof(PaddedCounter) == 64);

// Eight atomics packed into one 64-byte line. Same geometry as SharedLine but
// every increment now lowers to `lock xadd` — serialization + MESI combined.
struct alignas(64) SharedAtomicLine
{
  std::atomic<uint64_t> counters[8];
};
static_assert(sizeof(SharedAtomicLine) == 64);

// File-scope storage. Zero-initialized; shared across all benchmark runs.
// The "correct" final value does not matter — we are measuring throughput of
// the increment, not the sum.
static SharedLine g_shared_line{};
static PaddedCounter g_padded_counters[8]{};
static SharedAtomicLine g_shared_atomic_line{};
static SharedLine g_thread_local_publish{}; // keeps ThreadLocal's loop
                                            // from being elided

// ─────────────────────────────────────────────────────────────────────────────
// Variants
// ─────────────────────────────────────────────────────────────────────────────

static void BM_RawShared(benchmark::State& state)
{
  const int idx = state.thread_index();
  uint64_t* slot = &g_shared_line.counters[idx];
  for (auto _ : state)
  {
    ++(*slot);
    benchmark::DoNotOptimize(*slot);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

static void BM_RawPadded(benchmark::State& state)
{
  const int idx = state.thread_index();
  uint64_t* slot = &g_padded_counters[idx].value;
  for (auto _ : state)
  {
    ++(*slot);
    benchmark::DoNotOptimize(*slot);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

static void BM_AtomicShared(benchmark::State& state)
{
  const int idx = state.thread_index();
  std::atomic<uint64_t>* slot = &g_shared_atomic_line.counters[idx];
  for (auto _ : state)
  {
    slot->fetch_add(1, std::memory_order_relaxed);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

static void BM_ThreadLocal(benchmark::State& state)
{
  const int idx = state.thread_index();
  uint64_t local = 0;
  for (auto _ : state)
  {
    ++local;
    benchmark::DoNotOptimize(local);
  }
  // Publish once after the timed region so the optimizer cannot prove the
  // loop is dead. The publish itself is outside the timed region.
  g_thread_local_publish.counters[idx] = local;
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration — ThreadRange sweeps 1, 2, 4, 8 threads per variant.
// No working-set axis: false sharing is a single-cache-line phenomenon.
// ─────────────────────────────────────────────────────────────────────────────

BENCHMARK(BM_RawShared)->ThreadRange(1, 8)->UseRealTime();
BENCHMARK(BM_RawPadded)->ThreadRange(1, 8)->UseRealTime();
BENCHMARK(BM_AtomicShared)->ThreadRange(1, 8)->UseRealTime();
BENCHMARK(BM_ThreadLocal)->ThreadRange(1, 8)->UseRealTime();
BENCHMARK_MAIN();
