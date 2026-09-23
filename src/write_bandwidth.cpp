#include "size_literals.hpp"
#include <benchmark/benchmark.h>
#include <cstdint>
#include <immintrin.h>
#include <vector>

using namespace bench::literals;

// Write bandwidth benchmark — the suite's first write-bound test.
//
// Every other benchmark is read-bound (accumulators live in registers, the hot
// loop only loads). This one streams stores across an array and measures write
// bandwidth (GB/s).
//
// Plain store. x86 caches are write-allocate + write-back, so a store to a line
// that is not already cached triggers a Read-For-Ownership (RFO): the CPU fetches
// the whole 64-byte line from memory just to overwrite it. A pure write stream
// therefore secretly pays read traffic too — the asymmetry this benchmark exists
// to show.
static void BM_PlainStore(benchmark::State& state)
{
  const size_t size = state.range(0);
  const size_t n = size / sizeof(uint64_t);
  std::vector<uint64_t> data(n, 0);

  for (auto _ : state)
  {
    for (size_t i = 0; i < n; ++i)
    {
      data[i] = 0x42;
    }
    benchmark::DoNotOptimize(data.data());
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(state.iterations() * size);
  state.counters["size_kb"] = size / 1024.0;
}

// Non-temporal (streaming) store. Same write stream, but _mm_stream_si64 emits
// movnti, which bypasses the cache through write-combining buffers — no
// Read-For-Ownership and no cache pollution. Past L2/L3 that roughly halves the
// memory traffic (write-only, instead of the plain store's RFO read + write), so
// it should ~2x the plain store there; inside cache it LOSES, because it skips
// the hot line a plain store would simply overwrite. _mm_sfence() commits the
// weakly-ordered NT stores before the timed iteration ends. Unlike the plain
// store, the compiler cannot fold this into memset, so the movnti path is
// guaranteed.
static void BM_NonTemporalStore(benchmark::State& state)
{
  const size_t size = state.range(0);
  const size_t n = size / sizeof(uint64_t);
  std::vector<uint64_t> data(n, 0);

  for (auto _ : state)
  {
    for (size_t i = 0; i < n; ++i)
    {
      _mm_stream_si64(reinterpret_cast<long long*>(&data[i]), 0x42);
    }

    _mm_sfence();

    benchmark::DoNotOptimize(data.data());
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(state.iterations() * size);
  state.counters["size_kb"] = size / 1024.0;
}

// Sweep 4KB to 256MB — crosses L1 -> L2 -> L3 -> RAM, same as the read benchmarks.
BENCHMARK(BM_PlainStore)->RangeMultiplier(2)->Range(4_KB, 256_MB)->UseRealTime();
BENCHMARK(BM_NonTemporalStore)->RangeMultiplier(2)->Range(4_KB, 256_MB)->UseRealTime();
BENCHMARK_MAIN();
