#!/usr/bin/env python3
"""Tile the per-benchmark canonical plots into one small-multiples grid.

Each benchmark reports a different metric (GB/s, ns/op, GFLOPS), so a single
shared-axis chart would be dishonest. Instead this arranges the existing
canonical PNGs side by side — the whole suite "at a glance", each panel keeping
its own axis and units.

Usage:
    python3 scripts/plot_summary_grid.py            # -> results/summary_grid.png
    python3 scripts/plot_summary_grid.py -o out.png
"""

import argparse
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.image as mpimg

# (title, path-to-canonical-png) in reading order.
PANELS = [
    ("Sequential — bandwidth (GB/s)", "results/sequential/canonical.png"),
    ("Strided — bandwidth (GB/s)", "results/strided/canonical.png"),
    ("Pointer chasing — latency (ns/op)", "results/pointer_chasing/canonical.png"),
    ("AoS vs SoA — bandwidth (GB/s)", "results/aos_vs_soa/v4_canonical.png"),
    ("False sharing — ns/increment vs threads", "results/false_sharing/canonical.png"),
    ("Matrix multiply — GFLOPS", "results/matrix_multiply/canonical.png"),
    ("Write bandwidth — plain vs NT (GB/s)", "results/write_bandwidth/canonical.png"),
]


def main():
    ap = argparse.ArgumentParser(description="Assemble the summary grid")
    ap.add_argument("--output", "-o", default="results/summary_grid.png")
    args = ap.parse_args()

    cols, rows = 2, 4
    fig, axes = plt.subplots(rows, cols, figsize=(16, 20))
    axes = axes.ravel()

    for ax, (title, path) in zip(axes, PANELS):
        p = Path(path)
        if p.exists():
            ax.imshow(mpimg.imread(p))
            ax.set_title(title, fontsize=13)
        else:
            ax.text(0.5, 0.5, f"missing:\n{path}", ha="center", va="center", fontsize=11)
            ax.set_title(title, fontsize=13)
        ax.axis("off")

    for ax in axes[len(PANELS):]:
        ax.axis("off")

    fig.suptitle("Cache Benchmarks — the suite at a glance", fontsize=18, y=0.995)
    plt.tight_layout(rect=(0, 0, 1, 0.99))
    plt.savefig(args.output, dpi=110, bbox_inches="tight")
    print(f"Saved to {args.output}")


if __name__ == "__main__":
    main()
