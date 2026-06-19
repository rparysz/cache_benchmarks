#!/usr/bin/env python3
"""Compare the tiling margin at two CPU clocks (matmul only).

Overlays `tiled` and `restrict` GFLOPS-vs-working-set for a full-turbo run and a
fixed-base-clock (turbo-off) run. The point it makes: the gap between tiled and
restrict is *wider* at full turbo — a faster CPU outruns RAM harder, so it is
more memory-bound, so the reuse win (tiling) matters more. Turbo solid, base
dashed; the L3 line marks where the cliff begins.

Usage:
    python3 scripts/plot_clock_comparison.py \
        --turbo results/matrix_multiply/canonical.json \
        --base  results/matrix_multiply/no_turbo.json \
        -o results/matrix_multiply/clock_comparison.png
"""

import argparse
import json
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# The two variants whose gap is the story. Matched to the ->Name() labels.
VARIANTS = ["tiled (blocked + SIMD)", "ikj + restrict (SIMD)"]
COLORS = {"tiled (blocked + SIMD)": "#7E57C2", "ikj + restrict (SIMD)": "#43A047"}
SHORT = {"tiled (blocked + SIMD)": "tiled", "ikj + restrict (SIMD)": "restrict"}
L3_KB = 6144


def series_for(path: Path, variant: str):
    """Return (working_set_kb[], gflops[]) for one variant, sorted by size."""
    data = json.loads(path.read_text())
    means = [b for b in data["benchmarks"] if b.get("aggregate_name") == "mean"]
    rows = means or [b for b in data["benchmarks"] if b.get("run_type") != "aggregate"]
    pts = []
    for b in rows:
        if b["name"].split("/")[0] != variant:
            continue
        n = int(b.get("N", 0))
        pts.append((3 * n * n * 8 / 1024, b.get("FLOPS", 0) / 1e9))
    pts.sort()
    return [p[0] for p in pts], [p[1] for p in pts]


def main():
    ap = argparse.ArgumentParser(description="Compare tiling margin at two clocks")
    ap.add_argument("--turbo", required=True, help="full-turbo JSON")
    ap.add_argument("--base", required=True, help="fixed-base-clock (turbo-off) JSON")
    ap.add_argument("--output", "-o", help="save to file instead of showing")
    args = ap.parse_args()

    fig, ax = plt.subplots(figsize=(11, 6))

    for label, path, style in [("turbo", Path(args.turbo), "-"), ("base clock", Path(args.base), "--")]:
        for variant in VARIANTS:
            xs, ys = series_for(path, variant)
            if not xs:
                continue
            short = SHORT[variant]  # "tiled" / "restrict"
            ax.plot(
                xs, ys, style, marker="o", markersize=5, linewidth=1.8,
                color=COLORS[variant], alpha=1.0 if style == "-" else 0.55,
                label=f"{short} ({label})",
            )

    ax.axvline(x=L3_KB, color="#F44336", linestyle="--", linewidth=1.2, alpha=0.8)
    ax.text(L3_KB * 1.05, ax.get_ylim()[1] * 0.98, "L3 (6MB)", color="#F44336", fontsize=9, va="top")

    ax.set_xscale("log", base=2)
    ax.set_xlabel("Working Set Size", fontsize=12)
    ax.set_ylabel("GFLOPS", fontsize=12)
    ax.set_title("Tiling margin: full turbo (solid) vs fixed base clock (dashed)", fontsize=14)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=10)

    def kb_fmt(x, _):
        return f"{x/1024:.0f}MB" if x >= 1024 else f"{x:.0f}KB"

    ax.xaxis.set_major_formatter(ticker.FuncFormatter(kb_fmt))
    plt.tight_layout()

    if args.output:
        plt.savefig(args.output, dpi=150)
        print(f"Saved to {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
