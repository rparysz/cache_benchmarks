#include "size_literals.hpp"
#include <benchmark/benchmark.h>
#include <cstdint>
#include <vector>

using namespace bench::literals;

// AoS vs SoA benchmark — demonstrates the impact of data layout on cache utilization.
//
// Simulates a common pattern: a collection of "particles" with position (x,y,z)
// and velocity (vx,vy,vz), where a hot loop only needs the x coordinates.
//
// AoS (Array of Structures): each particle is a 48-byte struct. Iterating over
//   just the x field touches 8 bytes per 48 bytes fetched — 83% of every cache
//   line is wasted on fields the loop never reads.
//
// SoA (Structure of Arrays): x coordinates are packed in a contiguous array.
//   Every byte fetched is used — full cache line utilization, same as sequential.
//
// Methodology: the `size` parameter represents the *hot data footprint* — the
// bytes that actually exert cache pressure. AoS computes n = size / 48 because
// each particle pulls a full 48 bytes into cache. SoA computes n = size / 8
// because only the x array is touched. With this convention both benchmarks
// produce the same cache pressure at the same `size`, so L1d/L2/L3/RAM
// transitions line up on the x axis of the plot.
//
// The performance gap grows at L2+ where memory bandwidth is the bottleneck,
// matching the spatial locality waste pattern observed in the strided benchmark.

struct Particle
{
  uint64_t x, y, z;
  uint64_t vx, vy, vz;
};
// sizeof(Particle) = 48 bytes → ~1.33 particles per cache line
// Accessing only x: 8 useful bytes per 48 bytes touched = 16.7% utilization
// Using uint64_t (not double) removes the FP compute ceiling so that L1d/L2/L3
// numbers reflect memory-hierarchy behavior rather than FP add latency.
static void BM_AoS(benchmark::State& state)
{
  const size_t size = state.range(0);
  const size_t n = size / sizeof(Particle);
  const size_t limit = n - (n % 8); // round down so unrolled loop has no tail
  std::vector<Particle> particles(n);

  for (size_t i = 0; i < n; ++i)
  {
    particles[i] = {static_cast<uint64_t>(i), 0, 0, 0, 0, 0};
  }

  uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;

  for (auto _ : state)
  {
    for (size_t i = 0; i < limit; i += 8)
    {
      s0 += particles[i + 0].x;
      s1 += particles[i + 1].x;
      s2 += particles[i + 2].x;
      s3 += particles[i + 3].x;
      s4 += particles[i + 4].x;
      s5 += particles[i + 5].x;
      s6 += particles[i + 6].x;
      s7 += particles[i + 7].x;
    }
    benchmark::DoNotOptimize(s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7);
  }

  state.SetBytesProcessed(state.iterations() * limit * sizeof(uint64_t));
  state.counters["particles"] = static_cast<double>(n);
}

struct ParticleArrays
{
  std::vector<uint64_t> x, y, z;
  std::vector<uint64_t> vx, vy, vz;

  explicit ParticleArrays(size_t n)
      : x(n)
      , y(n)
      , z(n)
      , vx(n)
      , vy(n)
      , vz(n)
  {}
};

static void BM_SoA(benchmark::State& state)
{
  const size_t size = state.range(0);
  // Size by *hot data footprint*, not by particle count. AoS touches 48 B per
  // particle (full cache line fill), SoA touches only sizeof(uint64_t) = 8 B.
  // So for the same cache pressure `size`, SoA iterates over 6x more particles.
  const size_t n = size / sizeof(uint64_t);
  const size_t limit = n - (n % 8);
  ParticleArrays particles(n);

  for (size_t i = 0; i < n; ++i)
  {
    particles.x[i] = static_cast<uint64_t>(i);
  }

  uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;

  for (auto _ : state)
  {
    for (size_t i = 0; i < limit; i += 8)
    {
      s0 += particles.x[i + 0];
      s1 += particles.x[i + 1];
      s2 += particles.x[i + 2];
      s3 += particles.x[i + 3];
      s4 += particles.x[i + 4];
      s5 += particles.x[i + 5];
      s6 += particles.x[i + 6];
      s7 += particles.x[i + 7];
    }
    benchmark::DoNotOptimize(s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7);
  }

  state.SetBytesProcessed(state.iterations() * limit * sizeof(uint64_t));
  state.counters["particles"] = static_cast<double>(n);
}

static void BM_SoA16(benchmark::State& state)
{
  const size_t size = state.range(0);
  const size_t n = size / sizeof(uint64_t);
  const size_t limit = n - (n % 16);
  ParticleArrays particles(n);
  for (size_t i = 0; i < n; ++i) particles.x[i] = static_cast<uint64_t>(i);

  uint64_t s0=0,s1=0,s2=0,s3=0,s4=0,s5=0,s6=0,s7=0;
  uint64_t s8=0,s9=0,sa=0,sb=0,sc=0,sd=0,se=0,sf=0;

  for (auto _ : state)
  {
    for (size_t i = 0; i < limit; i += 16)
    {
      s0 += particles.x[i+0];  s1 += particles.x[i+1];
      s2 += particles.x[i+2];  s3 += particles.x[i+3];
      s4 += particles.x[i+4];  s5 += particles.x[i+5];
      s6 += particles.x[i+6];  s7 += particles.x[i+7];
      s8 += particles.x[i+8];  s9 += particles.x[i+9];
      sa += particles.x[i+10]; sb += particles.x[i+11];
      sc += particles.x[i+12]; sd += particles.x[i+13];
      se += particles.x[i+14]; sf += particles.x[i+15];
    }
    benchmark::DoNotOptimize(s0+s1+s2+s3+s4+s5+s6+s7+s8+s9+sa+sb+sc+sd+se+sf);
  }
  state.SetBytesProcessed(state.iterations() * limit * sizeof(uint64_t));
  state.counters["particles"] = static_cast<double>(n);
}

BENCHMARK(BM_AoS)->RangeMultiplier(2)->Range(4_KB, 256_MB)->UseRealTime();
BENCHMARK(BM_SoA)->RangeMultiplier(2)->Range(4_KB, 256_MB)->UseRealTime();
BENCHMARK(BM_SoA16)->RangeMultiplier(2)->Range(4_KB, 256_MB)->UseRealTime();

BENCHMARK_MAIN();
