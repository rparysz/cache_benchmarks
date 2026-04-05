#include "size_literals.hpp"
#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

using namespace bench::literals;

// ─────────────────────────────────────────────────────────────────────────────
// Pointer-chasing benchmark — replicates Drepper paper Section 3.2 (Figs 3.10–3.15)
//
// We build a circular linked list and traverse it by following pointers.
// Two layouts:
//   Sequential: nodes sit in memory order (0→1→2→…→N-1→0)
//               The prefetcher CAN recognise the stride and fetch ahead.
//   Random:     nodes are linked in a shuffled order.
//               The prefetcher CANNOT predict the next address — blind.
//
// NPAD controls how much padding each node carries, changing the node size:
//   NPAD=0  →  8  bytes/node → 8 nodes per 64-byte cache line
//   NPAD=7  → 64  bytes/node → 1 node per cache line  (exactly)
//   NPAD=15 → 128 bytes/node → 1 node per 2 cache lines
//   NPAD=31 → 256 bytes/node → 1 node per 4 cache lines
//
// Key question this benchmark answers:
//   How many cycles does it cost to follow one pointer, and how does that
//   change as the working set grows past L1 → L2 → L3 → RAM?
// ─────────────────────────────────────────────────────────────────────────────

// Node with NPAD long-sized padding words after the pointer.
// On 64-bit Linux: pointer = 8 bytes, long = 8 bytes.
template <size_t NPAD>
struct Node
{
  Node* next;
  long pad[NPAD];
};

// Specialisation for NPAD=0: just the pointer, no padding array.
template <>
struct Node<0>
{
  Node<0>* next;
};

// ─────────────────────────────────────────────────────────────────────────────
// List builders
// ─────────────────────────────────────────────────────────────────────────────

// Sequential: link nodes in memory order → 0→1→2→…→N-1→0.
// The stride between nodes is exactly sizeof(Node<NPAD>).
// The prefetcher can detect this regular stride and fetch ahead.
template <size_t NPAD>
Node<NPAD>* build_sequential(std::vector<Node<NPAD>>& nodes)
{
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    nodes[i].next = &nodes[(i + 1) % nodes.size()]; // Make it circular by linking the last node back to the first.
  }

  return &nodes[0];
}

// Random: shuffle the visit order, then link nodes to follow that order.
// Every pointer now points to an unpredictable location → prefetcher is blind.
// Fixed seed (42) for reproducibility.
template <size_t NPAD>
Node<NPAD>* build_random(std::vector<Node<NPAD>>& nodes)
{
  std::vector<size_t> order(nodes.size());
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), std::mt19937{42});

  for (size_t i = 0; i < order.size(); ++i)
  {
    nodes[order[i]].next = &nodes[order[(i + 1) % order.size()]]; // Make it circular by linking the last node back to the first.
  }

  return &nodes[order[0]];
}

// ─────────────────────────────────────────────────────────────────────────────
// The benchmark
// ─────────────────────────────────────────────────────────────────────────────

template <size_t NPAD, bool Random>
static void BM_PointerChase(benchmark::State& state)
{
  const size_t working_set = static_cast<size_t>(state.range(0));
  const size_t node_size = sizeof(Node<NPAD>);

  // How many nodes fit in the requested working set?
  // At least 2 so the list is always a valid cycle.
  const size_t num_nodes = std::max<size_t>(working_set / node_size, 2);

  std::vector<Node<NPAD>> nodes(num_nodes);
  Node<NPAD>* head = Random ? build_random(nodes) : build_sequential(nodes);

  for (auto _ : state)
  {
    // Traverse the full list once: follow num_nodes pointers.
    // Each step loads the next pointer from wherever it lives in memory.
    // The compiler cannot eliminate this loop because it does not know
    // what value head->next->next->... holds at compile time.
    const Node<NPAD>* p = head;
    for (size_t i = 0; i < num_nodes; ++i)
    {
      p = p->next;
    }

    // Tell the compiler that p is "used" so it cannot optimise the loop away.
    benchmark::DoNotOptimize(p);
  }

  // SetItemsProcessed lets Google Benchmark compute and display ns/item,
  // which maps directly to "cycles per pointer dereference" (approx).
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(num_nodes));

  state.counters["node_B"] = static_cast<double>(node_size);
  state.counters["nodes"] = static_cast<double>(num_nodes);
  state.counters["set_KB"] = static_cast<double>(working_set) / 1024.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Register all NPAD × layout combinations.
// Working set range: 4 KB → 256 MB (crosses L1 → L2 → L3 → RAM on this machine).
// ─────────────────────────────────────────────────────────────────────────────

// clang-format off
#define REGISTER_CHASE(NPAD)                                    \
  BENCHMARK(BM_PointerChase<NPAD, false>)                       \
    ->Name("Sequential/NPAD=" #NPAD)                            \
    ->RangeMultiplier(2)                                        \
    ->Range(4_KB, 256_MB)                                        \
    ->UseRealTime();                                            \
  BENCHMARK(BM_PointerChase<NPAD, true>)                        \
    ->Name("Random/NPAD=" #NPAD)                                \
    ->RangeMultiplier(2)                                        \
    ->Range(4_KB, 256_MB)                                       \
    ->UseRealTime()
// clang-format on

REGISTER_CHASE(0);  // 8  bytes/node
REGISTER_CHASE(7);  // 64 bytes/node  — one full cache line
REGISTER_CHASE(15); // 128 bytes/node — two cache lines
REGISTER_CHASE(31); // 256 bytes/node — four cache lines

BENCHMARK_MAIN();
