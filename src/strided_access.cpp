#include "size_literals.hpp"
#include <benchmark/benchmark.h>
#include <cstdint>
#include <vector>

using namespace bench::literals;

// Strided read benchmark — measures spatial locality waste.
//
// Reads one byte per cache line (stride = 64 bytes), forcing a full 64-byte
// fetch for each 1-byte access. Reported bandwidth reflects only the bytes
// the program actually reads (size / 64), not the bytes the hardware fetches
// (size), making the cost of poor spatial locality directly visible.
//
// A single accumulator is used intentionally. The serial dependency chain
// (~4 cycle add latency) makes L1d measurements partially compute-bound,
// but L2+ results are memory-bound and representative — memory latency at
// those levels (12-250+ cycles) far exceeds the add cost. The simplicity
// of one accumulator keeps the code readable since the benchmark's purpose
// is illustrating spatial waste, not maximizing throughput.
static void BM_Strided(benchmark::State& state)
{
  const size_t size = state.range(0);
  const size_t stride = 64; // one cache line between each access
  std::vector<uint8_t> data(size, 1);

  for (auto _ : state)
  {
    uint64_t sum = 0;
    for (size_t i = 0; i < size; i += stride)
    {
      sum += data[i];
    }
    benchmark::DoNotOptimize(sum);
  }

  state.SetBytesProcessed(state.iterations() * (size / stride));
  state.counters["size_kb"] = size / 1024.0;
}

// Sweep from 4KB to 256MB — this range crosses L1 → L2 → L3 → RAM
BENCHMARK(BM_Strided)->RangeMultiplier(2)->Range(4_KB, 256_MB)->UseRealTime();
BENCHMARK_MAIN();