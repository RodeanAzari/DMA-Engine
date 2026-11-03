#!/usr/bin/env python3
"""Turn raw results/*.csv.gz into percentile tables and plots.

Replaces the generated block of docs/results.md and writes docs/imgs/latency-*.png.
Statistics live here, never in the measurement loop (CLAUDE.md section 8.2).

Usage: scripts/plot-latency.py        (from the repo root; needs matplotlib)
"""
import csv
import gzip
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter, NullLocator  # noqa: E402

RESULTS = Path("results")
DOC = Path("docs/results.md")
IMGS = Path("docs/imgs")
BEGIN, END = "<!-- BEGIN GENERATED -->", "<!-- END GENERATED -->"

PATHS = ["blocking", "kernel_poll", "user_poll"]
PATH_LABEL = {
    "blocking": "Blocking ioctl",
    "kernel_poll": "Kernel poll",
    "user_poll": "No-wait + user poll",
}
SIZES = [64, 256, 1024, 4096, 65536]
PCTS = [50, 90, 99, 99.9]

DATASETS = [
    ("host-run1", "Host kernel, simulated engine (T490, real silicon)"),
    ("guest-sync", "Guest kernel under QEMU TCG, synchronous device (`completion-delay-ns=0`)"),
    ("guest-delay5us", "Guest kernel under QEMU TCG, **includes an artificial 5 µs device delay** (`completion-delay-ns=5000`)"),
]

# Categorical slots 1-3 of the dataviz reference palette; fixed per path
COLOR = {"blocking": "#2a78d6", "kernel_poll": "#eb6834", "user_poll": "#1baf7a"}
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"


def load(name):
    """{(path, size): [(user_ns, kernel_ns), ...]} in measurement order."""
    pairs = defaultdict(list)
    with gzip.open(RESULTS / f"{name}.csv.gz", "rt", newline="") as f:
        for row in csv.DictReader(f):
            pairs[row["path"], int(row["size"])].append(
                (int(row["user_ns"]), int(row["kernel_ns"])))
    return pairs


def pct(ordered, p):
    """Nearest-rank percentile of an already sorted list."""
    return ordered[max(0, math.ceil(p / 100 * len(ordered)) - 1)]


def us(ns):
    return f"{ns / 1000:,.2f}"


def size_label(size):
    return f"{size // 1024} KiB" if size >= 1024 else f"{size} B"


def percentile_table(data):
    out = ["| Path | Size | p50 | p90 | p99 | p99.9 | max |",
           "|---|---:|---:|---:|---:|---:|---:|"]
    for path in PATHS:
        for size in SIZES:
            s = sorted(u for u, _ in data[path, size])
            cells = " | ".join(us(pct(s, p)) for p in PCTS)
            out.append(f"| {PATH_LABEL[path]} | {size_label(size)} | {cells} | {us(s[-1])} |")
    return out


def saving_table(data):
    out = ["| Size | Blocking p50 | User poll p50 | p50 change | Blocking p99 | User poll p99 | p99 change |",
           "|---:|---:|---:|---:|---:|---:|---:|"]
    for size in SIZES:
        b = sorted(u for u, _ in data["blocking", size])
        p = sorted(u for u, _ in data["user_poll", size])
        row = [size_label(size)]
        for q in (50, 99):
            bq, pq = pct(b, q), pct(p, q)
            row += [us(bq), us(pq), f"{(pq - bq) / bq * 100:+.0f}%"]
        out.append("| " + " | ".join(row) + " |")
    return out


def overhead_table(data):
    """Time outside the driver's submit-to-wake window, paired per sample."""
    out = ["| Path | Size | Driver-measured p50 | Rest of the ioctl, p50 |",
           "|---|---:|---:|---:|"]
    for path in ("blocking", "kernel_poll"):
        for size in SIZES:
            pairs = data[path, size]
            driver = sorted(k for _, k in pairs)
            rest = sorted(u - k for u, k in pairs)
            out.append(f"| {PATH_LABEL[path]} | {size_label(size)} | "
                       f"{us(pct(driver, 50))} | {us(pct(rest, 50))} |")
    return out


def repeat_table(a, b):
    out = ["| Path | Size | Run 1 p50 | Run 2 p50 | Difference | Run 1 p99 | Run 2 p99 | Difference |",
           "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for path in PATHS:
        for size in SIZES:
            sa = sorted(u for u, _ in a[path, size])
            sb = sorted(u for u, _ in b[path, size])
            row = [PATH_LABEL[path], size_label(size)]
            for q in (50, 99):
                qa, qb = pct(sa, q), pct(sb, q)
                row += [us(qa), us(qb), f"{(qb - qa) / qa * 100:+.0f}%"]
            out.append("| " + " | ".join(row) + " |")
    return out


def style(ax):
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=8, length=0, which="both")


MICROS = FuncFormatter(lambda v, _: f"{v:g}")


def plot_scaling(host, guest):
    """Median and tail against size: magnitude over an ordered axis, so lines."""
    fig, axes = plt.subplots(2, 2, figsize=(10, 7), sharex=True, facecolor=SURFACE)
    panels = [(host, "Host kernel, simulated engine"),
              (guest, "Guest under QEMU, synchronous device")]
    for col, (data, title) in enumerate(panels):
        for row, q in enumerate((50, 99)):
            ax = axes[row][col]
            style(ax)
            ax.grid(True, axis="y", color=GRID, linewidth=0.8)
            ax.set_axisbelow(True)
            for path in PATHS:
                ys = [pct(sorted(u for u, _ in data[path, s]), q) / 1000 for s in SIZES]
                ax.plot(SIZES, ys, color=COLOR[path], linewidth=2, marker="o",
                        markersize=6, markeredgecolor=SURFACE, markeredgewidth=1.5,
                        label=PATH_LABEL[path])
            ax.set_xscale("log", base=2)
            ax.set_yscale("log")
            ax.yaxis.set_major_formatter(MICROS)
            ax.yaxis.set_minor_formatter(MICROS)
            ax.set_xticks(SIZES, [size_label(s) for s in SIZES])
            ax.xaxis.set_minor_locator(NullLocator())
            ax.set_title(f"{title}: p{q}", loc="left", fontsize=10, color=INK)
            if col == 0:
                ax.set_ylabel("Latency (µs, log scale)", fontsize=8, color=INK2)
            if row == 1:
                ax.set_xlabel("Transfer size", fontsize=8, color=INK2)
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=3, frameon=False,
               fontsize=9, labelcolor=INK, bbox_to_anchor=(0.5, 0.95))
    fig.suptitle("Submission path latency by transfer size", x=0.06, ha="left",
                 fontsize=13, color=INK)
    fig.text(0.06, 0.015, "Software path only: syscall, wake-up and ring overhead. "
             "Not DMA hardware latency. Queue depth 1.", fontsize=8, color=INK2)
    fig.tight_layout(rect=(0, 0.035, 1, 0.92))
    fig.savefig(IMGS / "latency-by-size.png", dpi=200, facecolor=SURFACE)
    plt.close(fig)


def plot_histograms(data, name, title):
    """One histogram per path and size, to p99.9 so a single outlier cannot flatten it."""
    fig, axes = plt.subplots(len(SIZES), len(PATHS), figsize=(11, 9), sharex=True,
                             facecolor=SURFACE)
    all_vals = [u / 1000 for pairs in data.values() for u, _ in pairs]
    all_vals.sort()
    lo, hi = max(all_vals[0], 0.1), pct(all_vals, 99.9)
    edges = [lo * (hi / lo) ** (i / 80) for i in range(81)]
    for r, size in enumerate(SIZES):
        for c, path in enumerate(PATHS):
            ax = axes[r][c]
            style(ax)
            vals = sorted(u / 1000 for u, _ in data[path, size])
            ax.hist(vals, bins=edges, color=COLOR[path],
                    weights=[100 / len(vals)] * len(vals), rwidth=0.8)
            ax.set_xscale("log")
            ax.xaxis.set_major_formatter(MICROS)
            p50 = pct(vals, 50)
            ax.axvline(p50, color=INK, linewidth=0.8)
            ax.text(0.98, 0.82, f"p50 {p50:,.1f} µs", transform=ax.transAxes,
                    ha="right", fontsize=7.5, color=INK)
            if r == 0:
                ax.set_title(PATH_LABEL[path], loc="left", fontsize=10, color=INK)
            if c == 0:
                ax.set_ylabel(f"{size_label(size)}\n% of samples", fontsize=8, color=INK2)
            if r == len(SIZES) - 1:
                ax.set_xlabel("Latency (µs, log scale)", fontsize=8, color=INK2)
    fig.suptitle(title, x=0.06, ha="left", fontsize=13, color=INK)
    fig.text(0.06, 0.012, "10,000 samples per panel, x-axis cut at the p99.9 of all samples. "
             "Software path only, not DMA hardware latency.", fontsize=8, color=INK2)
    fig.tight_layout(rect=(0, 0.03, 1, 0.97))
    fig.savefig(IMGS / f"latency-hist-{name}.png", dpi=200, facecolor=SURFACE)
    plt.close(fig)


def main():
    data = {name: load(name) for name, _ in DATASETS}
    host2, guest2 = load("host-run2"), load("guest-sync-run2")

    lines = []
    for name, title in DATASETS:
        lines += [f"### {title}", "", "All values in µs, measured in userspace.", ""]
        lines += percentile_table(data[name]) + [""]
    lines += ["### Blocking ioctl vs. no-wait + user poll", ""]
    for name, title in DATASETS[:2]:
        lines += [f"**{title}**, run 1, µs", ""] + saving_table(data[name]) + [""]
    lines += ["**Host, run 2**, µs", ""] + saving_table(host2) + [""]
    lines += ["**Guest, synchronous, run 2**, µs", ""] + saving_table(guest2) + [""]
    lines += ["### Time inside and outside the driver's submit-to-wake window", ""]
    for name, title in DATASETS[:2]:
        lines += [f"**{title}**, µs", ""] + overhead_table(data[name]) + [""]
    lines += ["### Repeatability", "", "**Host, two runs on one module load**, µs", ""]
    lines += repeat_table(data["host-run1"], host2) + [""]
    lines += ["**Guest, synchronous, two separate boots**, µs", ""]
    lines += repeat_table(data["guest-sync"], guest2) + [""]

    text = DOC.read_text(encoding="utf-8")
    head, rest = text.split(BEGIN)
    _, tail = rest.split(END)
    DOC.write_text(f"{head}{BEGIN}\n" + "\n".join(lines) + f"\n{END}{tail}",
                   encoding="utf-8", newline="\n")

    IMGS.mkdir(parents=True, exist_ok=True)
    plot_scaling(data["host-run1"], data["guest-sync"])
    plot_histograms(data["host-run1"], "host", "Latency distribution: host kernel, simulated engine")
    plot_histograms(data["guest-sync"], "guest-sync", "Latency distribution: guest under QEMU, synchronous device")


if __name__ == "__main__":
    main()
