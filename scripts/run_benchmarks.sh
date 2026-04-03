#!/usr/bin/env bash
#
# Run one or more cache benchmarks under consistent, pinned conditions and
# capture the canonical JSON. With --plot, also (re)generate the canonical plot.
# Builds any missing binary first.
#
# Usage:
#   scripts/run_benchmarks.sh [--plot] [name ...]     # e.g. sequential
#   scripts/run_benchmarks.sh [--plot] all            # every known benchmark
#   scripts/run_benchmarks.sh                          # same as `all`
#
# Run conditions (recorded in each result JSON as aslr_enabled /
# cpu_scaling_enabled = false):
#   - taskset core pin  (single core; multi-threaded benchmarks override the core set)
#   - setarch -R        (ASLR off, for a repeatable memory layout)
# Assumes the CPU governor is already `performance` and the box is idle
# (check `uptime` — the 1-min load should be well under 1).

set -euo pipefail
cd "$(dirname "$0")/.."          # repo root
BUILD=build

# name -> "binary|taskset-cores|out-json|out-png|run-flags|plot-flags"
declare -A CFG=(
  [sequential]="sequential_access|3|results/sequential/8acc_canonical.json|results/sequential/canonical.png||"
)
ORDER=(sequential)

COMMON="--benchmark_out_format=json"

build_if_missing() {
  local bin="$1"
  if [[ ! -x "$BUILD/$bin" ]]; then
    echo ">> $bin not built — configuring/building..."
    [[ -d "$BUILD" ]] || cmake -S . -B "$BUILD" -G Ninja >/dev/null
    cmake --build "$BUILD" --target "$bin"
  fi
}

run_one() {
  local name="$1" spec bin cores out png rflags pflags
  spec="${CFG[$name]:-}"
  [[ -n "$spec" ]] || { echo "unknown benchmark: '$name' (known: ${ORDER[*]})" >&2; return 1; }
  IFS='|' read -r bin cores out png rflags pflags <<<"$spec"

  build_if_missing "$bin"
  mkdir -p "$(dirname "$out")"
  echo ">> running $name  (core $cores)  ->  $out"
  # shellcheck disable=SC2086  -- word-splitting of flag strings is intended
  taskset -c "$cores" setarch -R "./$BUILD/$bin" $COMMON $rflags --benchmark_out="$out"

  if [[ "$DO_PLOT" == 1 ]]; then
    echo ">> plotting $name  ->  $png"
    # shellcheck disable=SC2086
    python3 scripts/plot_results.py "$out" -o "$png" $pflags
  fi
  echo
}

# parse args: an optional --plot flag, then benchmark names (default: all)
DO_PLOT=0
targets=()
for a in "$@"; do
  case "$a" in
    --plot) DO_PLOT=1 ;;
    *) targets+=("$a") ;;
  esac
done
if [[ ${#targets[@]} -eq 0 || "${targets[0]}" == "all" ]]; then targets=("${ORDER[@]}"); fi

# advisory idle check — a noisy box makes the numbers unreliable
load=$(cut -d' ' -f1 /proc/loadavg)
if ! awk -v l="$load" 'BEGIN { exit (l+0 > 1.0) ? 1 : 0 }'; then
  echo "WARNING: 1-min load is $load (> 1). Results may be noisy — prefer an idle box." >&2
  echo
fi

for t in "${targets[@]}"; do run_one "$t"; done
echo "Done."
