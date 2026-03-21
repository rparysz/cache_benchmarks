#!/usr/bin/env python3
"""Plot cache benchmark results from Google Benchmark JSON output.

Auto-detects the metric in each file:
  - bytes_per_second  → bandwidth plot (GB/s)
  - items_per_second  → latency plot (ns/item)

Auto-detects the x-axis:
  - size sweep    → working-set size (KB/MB); cache boundary lines drawn
  - thread sweep  → thread count; no cache lines (benchmarks use ->Threads())

Usage:
    python3 scripts/plot_results.py results/sequential.json
    python3 scripts/plot_results.py results/pointer_chasing.json
    python3 scripts/plot_results.py results/false_sharing.json
    python3 scripts/plot_results.py results/sequential.json results/strided.json
"""

import json
import argparse
import re
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker


_THREADS_RX = re.compile(r"/threads:(\d+)")


def _parse_size_name(name: str) -> tuple[str, int]:
    """Return (series_label, size_bytes) from a size-sweep benchmark name.

    Handles both name formats:
      BM_Sequential/4096/real_time        → ("BM_Sequential", 4096)
      Sequential/NPAD=0/4096/real_time    → ("Sequential/NPAD=0", 4096)
    """
    base = name.replace("/real_time", "")
    parts = base.split("/")
    for i in range(len(parts) - 1, -1, -1):
        if parts[i].isdigit():
            label = "/".join(parts[:i])
            return label, int(parts[i])
    return base, 0


def _parse_threads_name(name: str) -> tuple[str, int]:
    """Return (variant, thread_count) from a thread-sweep benchmark name.

    Name format: BM_RawShared/real_time/threads:4 → ("BM_RawShared", 4)
    """
    variant = name.split("/", 1)[0]
    m = _THREADS_RX.search(name)
    return variant, int(m.group(1)) if m else 1


def parse_benchmark_json(path: Path):
    """Parse a benchmark JSON file.

    Returns:
        axis_mode   – "size" or "threads"
        y_label     – human-readable y-axis label
        series      – {label: ([x_value, ...], [y_value, ...])}
    """
    with open(path) as f:
        data = json.load(f)

    # Matmul-style: kernels emit a custom FLOPS counter and an N counter instead
    # of bytes_per_second / items_per_second. Plot GFLOPS on the y-axis, but keep
    # the same working-set-byte x-axis (3 * N*N * 8) and cache-boundary lines as
    # the rest of the suite, so the matmul plot reads in the same visual language.
    # With --benchmark_report_aggregates_only the file holds only aggregate rows,
    # so use the mean; fall back to raw iteration rows for a single-rep run.
    if any("FLOPS" in b for b in data["benchmarks"]):
        means = [b for b in data["benchmarks"] if b.get("aggregate_name") == "mean"]
        rows = means or [b for b in data["benchmarks"] if b.get("run_type") != "aggregate"]
        series: dict[str, tuple[list, list]] = {}
        for b in rows:
            label = b["name"].split("/")[0]
            n = int(b.get("N", 0))
            working_set_kb = 3 * n * n * 8 / 1024
            series.setdefault(label, ([], []))[0].append(working_set_kb)
            series[label][1].append(b.get("FLOPS", 0) / 1e9)
        sorted_series = {}
        for label, (xs, ys) in series.items():
            pairs = sorted(zip(xs, ys))
            sorted_series[label] = ([p[0] for p in pairs], [p[1] for p in pairs])
        return "size", "GFLOPS", sorted_series

    benchmarks = [b for b in data["benchmarks"] if b.get("run_type") != "aggregate"]

    axis_mode = "threads" if any("/threads:" in b["name"] for b in benchmarks) else "size"

    metric_key = "bytes_per_second"
    for b in benchmarks:
        if "bytes_per_second" in b:
            metric_key = "bytes_per_second"
            break
        if "items_per_second" in b:
            metric_key = "items_per_second"
            break

    y_label = "Bandwidth (GB/s)" if metric_key == "bytes_per_second" else "Latency (ns / item)"

    series: dict[str, tuple[list, list]] = {}
    for b in benchmarks:
        if axis_mode == "threads":
            label, x_val = _parse_threads_name(b["name"])
        else:
            label, size_bytes = _parse_size_name(b["name"])
            x_val = size_bytes / 1024  # KB on the x-axis

        if metric_key == "bytes_per_second":
            y_val = b.get("bytes_per_second", 0) / 1024**3
        else:
            ips = b.get("items_per_second", 0)
            y_val = (1e9 / ips) if ips else 0

        series.setdefault(label, ([], []))[0].append(x_val)
        series[label][1].append(y_val)

    # Sort each series by x ascending so plot lines connect left→right.
    sorted_series = {}
    for label, (xs, ys) in series.items():
        pairs = sorted(zip(xs, ys))
        sorted_series[label] = ([p[0] for p in pairs], [p[1] for p in pairs])

    return axis_mode, y_label, sorted_series


def main():
    parser = argparse.ArgumentParser(description="Plot cache benchmark results")
    parser.add_argument("files", nargs="*", help="JSON result files to plot")
    parser.add_argument("--output", "-o", help="Save plot to file instead of showing")
    parser.add_argument("--title", help="Override plot title")
    parser.add_argument("--logy", action="store_true", help="Log y-axis")
    parser.add_argument(
        "--label",
        action="append",
        default=[],
        help="NAME:PATH — add file with explicit legend prefix (repeatable)",
    )
    parser.add_argument(
        "--l1", type=float, default=32, help="L1 cache size in KB (default: 32)"
    )
    parser.add_argument(
        "--l2", type=float, default=256, help="L2 cache size in KB (default: 256)"
    )
    parser.add_argument(
        "--l3", type=float, default=6144, help="L3 cache size in KB (default: 6144)"
    )
    args = parser.parse_args()

    fig, ax = plt.subplots(figsize=(12, 6))

    inputs: list[tuple[str, str]] = []
    for spec in args.label:
        if ":" not in spec:
            raise SystemExit(f"--label expects NAME:PATH, got {spec!r}")
        name, path = spec.split(":", 1)
        inputs.append((name, path))
    positional = list(args.files)
    if len(positional) > 1:
        for path in positional:
            inputs.append((Path(path).stem, path))
    elif len(positional) == 1:
        inputs.append(("", positional[0]))
    if not inputs:
        raise SystemExit("No input files given (use positional args or --label)")

    axis_mode = "size"
    y_label = "Value"
    for prefix, path in inputs:
        axis_mode, y_label, series = parse_benchmark_json(Path(path))
        for label, (xs, vals) in series.items():
            if prefix and label:
                full = f"{prefix} ({label})" if len(series) > 1 else prefix
            else:
                full = prefix or label
            ax.plot(xs, vals, marker="o", markersize=5, linewidth=1.8, label=full)

    ax.set_xscale("log", base=2)
    if args.logy:
        ax.set_yscale("log")
    ax.set_ylabel(y_label, fontsize=12)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=10)

    if axis_mode == "threads":
        ax.set_xlabel("Threads", fontsize=12)
        # Explicit ticks at powers of two; ThreadRange usually stops at 8.
        all_xs = [x for _, (xs, _) in series.items() for x in xs]
        if all_xs:
            xmin, xmax = min(all_xs), max(all_xs)
            ticks = [1]
            while ticks[-1] < xmax:
                ticks.append(ticks[-1] * 2)
            ticks = [t for t in ticks if xmin <= t <= xmax]
            ax.set_xticks(ticks)
            ax.set_xticklabels([str(t) for t in ticks])
        default_title = "Throughput vs Thread Count" if "Bandwidth" in y_label else "Latency vs Thread Count"
        ax.set_title(args.title or default_title, fontsize=14)
    else:
        ax.set_xlabel("Working Set Size", fontsize=12)
        if "GFLOPS" in y_label:
            default_title = "Matrix Multiply — GFLOPS vs Working Set Size"
        elif "Latency" in y_label:
            default_title = "Memory Latency vs Working Set Size"
        else:
            default_title = "Memory Bandwidth vs Working Set Size"
        ax.set_title(args.title or default_title, fontsize=14)

        def kb_formatter(x, _):
            if x >= 1024:
                return f"{x/1024:.0f}MB"
            return f"{x:.0f}KB"

        ax.xaxis.set_major_formatter(ticker.FuncFormatter(kb_formatter))
        ax.xaxis.set_minor_formatter(ticker.FuncFormatter(kb_formatter))

        ylim = ax.get_ylim()
        for (label, size_kb), color in zip(
            [
                (f"L1 ({args.l1:.0f}KB)", args.l1),
                (f"L2 ({args.l2:.0f}KB)", args.l2),
                (f"L3 ({args.l3/1024:.0f}MB)", args.l3),
            ],
            ["#4CAF50", "#FF9800", "#F44336"],
        ):
            ax.axvline(x=size_kb, color=color, linestyle="--", linewidth=1.2, alpha=0.8)
            ax.text(
                size_kb * 1.05,
                ylim[1] * 0.98 if ylim[1] > 0 else 5,
                label,
                color=color,
                fontsize=9,
                va="top",
            )

    plt.tight_layout()

    if args.output:
        plt.savefig(args.output, dpi=150)
        print(f"Saved to {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
