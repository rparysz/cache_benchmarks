#include <algorithm>
#include <array>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdio>
#include <experimental/simd>
#include <functional>
#include <span>
#include <vector>

// ------------------------------------------------------------------------------------------------------------------------------

// Matrix multiply: C = A * B, square N x N, row-major double.
//
// The same multiply, built several ways, each targeting a different bottleneck. Read in order — each variant keeps
// the previous fix and adds one more:
//
//   matmul_ijk           naive textbook order. Inner loop walks B down a column (stride N) — cache-hostile. Baseline.
//   matmul_ikj           reordered to i,k,j so the inner loop walks B and C along rows (stride 1) — sequential and
//                        cache-friendly. Attacks the MEMORY access pattern.
//   matmul_ikj_restrict  identical math to ikj, but raw __restrict pointers promise A/B/C don't overlap, letting the
//                        compiler auto-vectorize (AVX2) instead of staying scalar. Attacks the COMPUTE bottleneck.
//                        std::span can't carry the no-alias promise, so the auto-vectorizer can't act.
//   matmul_ikj_simd      the same vectorization written BY HAND with std::experimental::simd — keeps the std::span
//                        signature yet still vectorizes.
//   matmul_tiled         cache-blocked ikj. Attacks REUSE: runs the multiply over cache-sized blocks so operands are
//                        reused while hot, cutting how many times they're refetched from RAM.
//
// The cache fix (loop order) and the SIMD fix (vectorization) aim at independent bottlenecks — memory access vs
// compute — so their speedups combine rather than overlap: we get both at once. Tiling adds a third independent
// lever, reuse. Which one dominates depends on where the working set sits relative to cache — see notes/ for the
// measured behaviour.
//
// Storage: each matrix is one contiguous N*N block of doubles, owned by a std::vector and viewed as a std::span
// (non-owning, knows its length). span is 1-D, so we index the row-major layout by hand: element (row i, col k) is at
// A[i*N + k]. (std::mdspan would give 2-D A[i,k] indexing, but libstdc++ 15 doesn't ship it yet — revisit when GCC adds it)
//
// Metric is GFLOPS: a multiply-add is 2 FLOPs and the algorithm does N^3 of them, so each call is 2*N^3 FLOPs —
// comparable across every variant. The working set is three matrices (3 * N*N * 8 bytes); sweeping N moves it through
// the cache hierarchy, which is what the variants are measured against.

// ------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------

// Naive ijk order. For each output cell C[i][j], walk row i of A and column j of B, summing the products. The inner
// loop reads B down a column (B[k*N + j], stride N) — cache-hostile, the strided pathology, and why this version is slow.
static void matmul_ijk(std::span<const double> A, std::span<const double> B, std::span<double> C, std::size_t N)
{
  for (auto i{0uz}; i < N; i++)
  {
    for (auto j{0uz}; j < N; j++)
    {
      double sum = 0.0;
      for (auto k{0uz}; k < N; k++)
      {
        sum += A[i * N + k] * B[k * N + j];
      }
      C[i * N + j] = sum;
    }
  }
}

// Cache-friendly ikj order. Reorder loops to i,k,j and hoist A[i][k] into a scalar. The inner loop over j now walks
// B[k][*] and C[i][*] along rows (stride 1) — sequential, cache-warm. Same N^3 multiply-adds as ijk, just a
// memory-friendlier order. C is built up with += across k, so it must start zeroed.
static void matmul_ikj(std::span<const double> A, std::span<const double> B, std::span<double> C, std::size_t N)
{
  for (auto i{0uz}; i < N; i++)
  {
    for (auto k{0uz}; k < N; k++)
    {
      double r = A[i * N + k];
      for (auto j{0uz}; j < N; j++)
      {
        C[i * N + j] += r * B[k * N + j];
      }
    }
  }
}

// ikj with __restrict — vectorizable. Identical math to matmul_ikj, but raw __restrict pointers promise A/B/C never
// overlap. Without that promise the compiler must assume B and C might alias and keeps the inner loop scalar; with it,
// the loop auto-vectorizes (AVX2: ymm / vfmadd*pd). This is why hot loops use __restrict, and why std::span (which
// can't carry the promise) leaves the auto-vectorizer no choice but scalar code. gnu::noinline keeps it a standalone
// symbol that's easy to find in objdump.
[[gnu::noinline]]
static void matmul_ikj_restrict(const double* __restrict A, const double* __restrict B, double* __restrict C, std::size_t N)
{
  for (auto i{0uz}; i < N; i++)
  {
    for (auto k{0uz}; k < N; k++)
    {
      double r = A[i * N + k];
      for (auto j{0uz}; j < N; j++)
      {
        C[i * N + j] += r * B[k * N + j];
      }
    }
  }
}

// ikj with explicit SIMD — same math and same ymm/vfmadd*pd as the __restrict version, but vectorized BY HAND via
// std::experimental::simd instead of relying on the auto-vectorizer. Because we emit the vector loads/stores ourselves,
// the aliasing doubt that kept the plain-span ikj scalar no longer blocks anything — so this keeps the safe std::span
// signature AND vectorizes. The cost is writing the split explicitly: a packed main loop plus a scalar remainder.
static void matmul_ikj_simd(std::span<const double> A, std::span<const double> B, std::span<double> C, std::size_t N)
{
  namespace stdx = std::experimental;
  using V = stdx::native_simd<double>;

  for (auto i{0uz}; i < N; i++)
  {
    for (auto k{0uz}; k < N; k++)
    {
      V vr(A[i * N + k]);
      std::size_t j = 0;

      // Packed loop: V::size() columns of C at a time, while a full chunk fits.
      for (; j + V::size() <= N; j += V::size())
      {
        V vb(&B[k * N + j], stdx::element_aligned);
        V vc(&C[i * N + j], stdx::element_aligned);
        vc += vr * vb;
        vc.copy_to(&C[i * N + j], stdx::element_aligned);
      }

      // Scalar remainder: the last N % V::size() columns
      for (; j < N; ++j)
      {
        C[i * N + j] += A[i * N + k] * B[k * N + j];
      }
    }
  }
}

// Cache-blocked (tiled) ikj — attacks REUSE, the bottleneck the others leave. restrict/simd fix how fast each pass
// streams, but still re-read all of B from RAM for every row of C. Tiling runs the multiply over T x T blocks small
// enough to stay cache-resident, so each block is reused across the whole tile before it's evicted — cutting how many
// times operands are refetched from RAM. Same __restrict inner kernel as matmul_ikj_restrict (so it still vectorizes);
// the only additions are the three outer tile loops and the min() clamps for edge tiles / small N.
//
// T trades off two effects: a bigger tile reuses each fetched element more times (fewer RAM passes), but the working
// set is 3 * T^2 * 8 bytes and must still fit a cache level with headroom, or the reuse itself starts missing. The best
// T is the largest that still fits; which level and value that is depends on the machine — see notes/ for the sweep.
static void matmul_tiled(const double* __restrict A, const double* __restrict B, double* __restrict C, std::size_t N)
{
  constexpr auto T{256uz}; // 3-block working set (3*T^2*8 B) sized to fit a cache level; see notes/

  for (auto ii{0uz}; ii < N; ii += T)
  {
    const auto i_max{std::min(ii + T, N)};
    for (auto jj{0uz}; jj < N; jj += T)
    {
      const auto j_max{std::min(jj + T, N)};
      for (auto kk{0uz}; kk < N; kk += T)
      {
        const auto k_max{std::min(kk + T, N)};

        for (auto i{ii}; i < i_max; i++)
        {
          for (auto k{kk}; k < k_max; k++)
          {
            const auto r = A[i * N + k];
            for (auto j{jj}; j < j_max; j++)
            {
              C[i * N + j] += r * B[k * N + j];
            }
          }
        }
      }
    }
  }
}

// Fill A and B with cheap, deterministic values. The exact numbers don't affect timing — only that the same data is
// used across runs and variants.
static void buildMatrices(std::span<double> A, std::span<double> B, std::size_t N)
{
  for (std::size_t i = 0; i < N * N; ++i)
  {
    A[i] = static_cast<double>(i % 7) + 1.0;
    B[i] = static_cast<double>(i % 5) + 1.0;
  }
}

// Common signature for a matmul kernel: read A and B, write C, square N x N. The fixture and verifier target this type
// so any variant drops in; raw-pointer kernels (e.g. the __restrict one) adapt through a small lambda.
using MatMulFunc = std::function<void(std::span<const double>, std::span<const double>, std::span<double>, const std::size_t)>;

static void benchmark_fixture(benchmark::State& state, MatMulFunc matmul_func)
{
  const std::size_t N = static_cast<std::size_t>(state.range(0));
  std::vector<double> Av(N * N), Bv(N * N), Cv(N * N, 0.0);

  buildMatrices(Av, Bv, N);

  for (auto _ : state)
  {
    std::fill(Cv.begin(), Cv.end(), 0.0); // C is recomputed each run
    matmul_func(Av, Bv, Cv, N);
    benchmark::DoNotOptimize(Cv.data()); // don't let the optimizer delete the work
    benchmark::ClobberMemory();          // "memory changed" barrier
  }

  // 2*N^3 FLOPs per call. kIsIterationInvariantRate = "this value happens once per iteration; divide by elapsed time"
  // -> a FLOPS rate. kIs1000 formats it with k / M / G prefixes (so you'll read it straight off as GFLOPS).
  state.counters["FLOPS"] = benchmark::Counter(2.0 * N * N * N, benchmark::Counter::kIsIterationInvariantRate, benchmark::Counter::kIs1000);
  state.counters["N"] = static_cast<double>(N);
}

static void BM_MatmulNaive(benchmark::State& state)
{
  benchmark_fixture(state, matmul_ijk);
}

static void BM_MatmulImproved(benchmark::State& state)
{
  benchmark_fixture(state, matmul_ikj);
}

static void BM_MatmulRestrict(benchmark::State& s)
{
  benchmark_fixture(s,
                    [](std::span<const double> A, std::span<const double> B, std::span<double> C, std::size_t N)
                    { matmul_ikj_restrict(A.data(), B.data(), C.data(), N); });
}

static void BM_MatmulSIMD(benchmark::State& s)
{
  benchmark_fixture(s, matmul_ikj_simd);
}

static void BM_MatmulTiled(benchmark::State& s)
{
  benchmark_fixture(s,
                    [](std::span<const double> A, std::span<const double> B, std::span<double> C, std::size_t N)
                    { matmul_tiled(A.data(), B.data(), C.data(), N); });
}

// Naive is capped at a smaller maximum — it grows ~N^3 and gets slow fast;
// the faster variants sweep further, out to sizes that spill cache.
// ->Name() sets the display/legend label (the plot reads it); the progression
// reads left-to-right as each optimization is added.
BENCHMARK(BM_MatmulNaive)->Name("naive (ijk)")->RangeMultiplier(2)->Range(64, 512)->UseRealTime();
BENCHMARK(BM_MatmulImproved)->Name("ikj (cache order)")->RangeMultiplier(2)->Range(64, 2048)->UseRealTime();
BENCHMARK(BM_MatmulRestrict)->Name("ikj + restrict (SIMD)")->RangeMultiplier(2)->Range(64, 2048)->UseRealTime();
BENCHMARK(BM_MatmulSIMD)->Name("ikj + simd (explicit)")->RangeMultiplier(2)->Range(64, 2048)->UseRealTime();
BENCHMARK(BM_MatmulTiled)->Name("tiled (blocked + SIMD)")->RangeMultiplier(2)->Range(64, 2048)->UseRealTime();

// Run a kernel on the hand-computed 2x2 (A={1,2,3,4}, B={5,6,7,8} -> C={19,22,43,50}) and compare. Small integers are
// exact in double, so == is fine here (a larger / real-FP check would need a tolerance). main() runs this for every
// kernel before any timing and aborts if one fails.
static bool verify_2x2(const char* name, MatMulFunc kernel)
{
  std::array<double, 4> a = {1, 2, 3, 4};
  std::array<double, 4> b = {5, 6, 7, 8};
  std::array<double, 4> expected = {19, 22, 43, 50};
  std::array<double, 4> c = {0, 0, 0, 0}; // mutable + zeroed — ikj accumulates with +=

  kernel(a, b, c, 2);

  bool ok = c[0] == expected[0] && c[1] == expected[1] && c[2] == expected[2] && c[3] == expected[3];
  std::printf("2x2 verify %-22s : C = %g %g %g %g  -> %s\n", name, c[0], c[1], c[2], c[3], ok ? "PASS" : "FAIL");
  return ok;
}

int main(int argc, char** argv)
{
  // Verify every kernel before trusting any timing; bail if one is wrong.
  bool ok = true;
  ok &= verify_2x2("naive (ijk)", matmul_ijk);
  ok &= verify_2x2("ikj (cache order)", matmul_ikj);
  ok &= verify_2x2("ikj + restrict (SIMD)",
                   [](std::span<const double> A, std::span<const double> B, std::span<double> C, std::size_t N)
                   { matmul_ikj_restrict(A.data(), B.data(), C.data(), N); });
  ok &= verify_2x2("ikj + simd (explicit)", matmul_ikj_simd);
  ok &= verify_2x2("tiled (blocked + SIMD)",
                   [](std::span<const double> A, std::span<const double> B, std::span<double> C, std::size_t N)
                   { matmul_tiled(A.data(), B.data(), C.data(), N); });
  if (!ok)
  {
    std::fprintf(stderr, "verify failed — aborting\n");
    return 1;
  }

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
