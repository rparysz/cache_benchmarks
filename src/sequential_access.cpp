#include "size_literals.hpp"
#include <benchmark/benchmark.h>
#include <cstdint>
#include <vector>

using namespace bench::literals;

// Sequential read bandwidth benchmark.
//
// We use 8 independent accumulators to break the serial dependency chain and
// let the compiler auto-vectorize into 2x vpaddq ymm (AVX2), processing 64
// bytes (one cache line) per loop iteration. This pushes the compute ceiling
// above L1d bandwidth so that memory is the bottleneck at every cache level.
//
// Fewer accumulators (1-4): compute-bound — serial dependency or insufficient
//   ILP causes a flat bandwidth curve that understates memory throughput.
// 16 accumulators: higher L1d peak (~132 GB/s vs ~102), no register spills
//   (4x vpaddq ymm fits in 16 ymm regs), but L2+ numbers match 8 acc.
// 32+ accumulators: register spills collapse performance to below 1-acc levels.
// 8 is the sweet spot for measuring memory bandwidth across all cache levels.
static void BM_Sequential(benchmark::State& state)
{
  const size_t size = state.range(0);
  const size_t n = size / sizeof(uint64_t);
  std::vector<uint64_t> data(n, 1);
  uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;

  for (auto _ : state)
  {
    for (size_t i = 0; i < n; i += 8)
    {
      s0 += data[i];
      s1 += data[i + 1];
      s2 += data[i + 2];
      s3 += data[i + 3];
      s4 += data[i + 4];
      s5 += data[i + 5];
      s6 += data[i + 6];
      s7 += data[i + 7];
    }
    benchmark::DoNotOptimize(s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7);
  }

  state.SetBytesProcessed(state.iterations() * size);
  state.counters["size_kb"] = size / 1024.0;
}

// Sweep from 4KB to 256MB — this range crosses L1 → L2 → L3 → RAM
BENCHMARK(BM_Sequential)->RangeMultiplier(2)->Range(4_KB, 256_MB)->UseRealTime();
BENCHMARK_MAIN();