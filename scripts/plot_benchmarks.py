#!/usr/bin/env python3
"""
Generate README-ready ExchangeLab benchmark figures.

Summary figures come from the committed CSV summaries under benchmark-results/.

For the real TCP latency distribution, this script automatically looks for
sample-level data in either of these files, in this order:

    benchmark-results/end_to_end_latency.csv
    benchmark-results/tcp_latency_samples.csv

The current ExchangeLab end-to-end latency benchmark already writes:
    sample,latency_ns,latency_us

No synthetic distribution is created from p50/p95/p99 values.
"""

from pathlib import Path
import csv

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "benchmark-results"
FIGURES = ROOT / "docs" / "figures"
FIGURES.mkdir(parents=True, exist_ok=True)


def read_csv(name):
    path = RESULTS / name
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def save_queue_throughput():
    rows = read_csv("queue_comparison.csv")
    labels = ["1P / 1C", "4P / 1C"]
    spsc = [float(r["sharded_spsc_messages_per_sec"]) / 1e6 for r in rows]
    mutex = [float(r["mutex_messages_per_sec"]) / 1e6 for r in rows]

    x = list(range(len(labels)))
    width = 0.36

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.bar([i - width / 2 for i in x], spsc, width, label="Sharded SPSC")
    ax.bar([i + width / 2 for i in x], mutex, width, label="Mutex queue")
    ax.set_xticks(x, labels)
    ax.set_ylabel("Throughput (million messages/sec)")
    ax.set_title("Market-data queue throughput")
    ax.legend()
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(FIGURES / "queue_throughput.png", dpi=180)
    plt.close(fig)


def save_queue_speedup():
    rows = read_csv("queue_comparison.csv")
    labels = ["1P / 1C", "4P / 1C"]
    speedups = [float(r["speedup"]) for r in rows]

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    bars = ax.bar(labels, speedups)
    ax.axhline(1.0, linewidth=1)
    ax.set_ylabel("Speedup vs mutex (×)")
    ax.set_title("Sharded SPSC speedup")
    ax.grid(axis="y", alpha=0.25)

    for bar, value in zip(bars, speedups):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height(),
            f"{value:.2f}×",
            ha="center",
            va="bottom",
        )

    fig.tight_layout()
    fig.savefig(FIGURES / "queue_speedup.png", dpi=180)
    plt.close(fig)


def save_matching_throughput():
    rows = read_csv("matching_throughput_summary.csv")
    labels = [r["scenario"] for r in rows]
    values = [float(r["orders_per_sec"]) / 1e6 for r in rows]

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    bars = ax.bar(labels, values)
    ax.set_ylabel("Orders/sec (millions)")
    ax.set_title("Matching engine throughput")
    ax.grid(axis="y", alpha=0.25)

    for bar, value in zip(bars, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height(),
            f"{value:.1f}M",
            ha="center",
            va="bottom",
        )

    fig.tight_layout()
    fig.savefig(FIGURES / "matching_throughput.png", dpi=180)
    plt.close(fig)


def save_latency_percentiles():
    rows = read_csv("latency_summary.csv")
    wanted = ["p50", "p95", "p99"]
    rows = [r for r in rows if r["metric"] in wanted]
    rows.sort(key=lambda r: wanted.index(r["metric"]))

    labels = [r["metric"] for r in rows]
    tcp = [float(r["tcp_us"]) for r in rows]
    execution = [float(r["execution_us"]) for r in rows]
    x = list(range(len(labels)))

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.plot(x, tcp, marker="o", label="End-to-end TCP")
    ax.plot(x, execution, marker="o", label="Execution")
    ax.set_xticks(x, labels)
    ax.set_ylabel("Latency (µs)")
    ax.set_title("Latency percentiles")
    ax.legend()
    ax.grid(alpha=0.25)
    fig.tight_layout()
    fig.savefig(FIGURES / "latency_percentiles.png", dpi=180)
    plt.close(fig)


def save_memory_pool_allocations():
    rows = read_csv("memory_pool_summary.csv")
    labels = [r["phase"] for r in rows]
    allocations = [int(r["upstream_allocations"]) for r in rows]

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    bars = ax.bar(labels, allocations)
    ax.set_ylabel("Upstream allocations")
    ax.set_title("Memory-pool allocator behavior")
    ax.set_ylim(0, max(5, max(allocations) + 1))
    ax.grid(axis="y", alpha=0.25)

    for bar, value in zip(bars, allocations):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 0.05,
            str(value),
            ha="center",
            va="bottom",
        )

    fig.tight_layout()
    fig.savefig(FIGURES / "memory_pool_allocations.png", dpi=180)
    plt.close(fig)


def find_latency_sample_file():
    candidates = [
        RESULTS / "end_to_end_latency.csv",
        RESULTS / "tcp_latency_samples.csv",
    ]

    for path in candidates:
        if path.exists():
            return path

    return None


def load_latency_samples(path):
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        fields = set(reader.fieldnames or [])

        if "latency_us" in fields:
            return [float(row["latency_us"]) for row in reader]

        if "latency_ns" in fields:
            return [float(row["latency_ns"]) / 1000.0 for row in reader]

        raise ValueError(
            f"{path} must contain either latency_us or latency_ns"
        )


def percentile(sorted_values, probability):
    if not sorted_values:
        return 0.0

    index = int(round((len(sorted_values) - 1) * probability))
    return sorted_values[index]


def save_latency_distribution():
    path = find_latency_sample_file()

    if path is None:
        print(
            "Skipping TCP latency histogram/CDF: no raw sample CSV found.\n"
            "Run ./build-release/exchange_lab_latency_benchmark first."
        )
        return

    samples = load_latency_samples(path)

    if not samples:
        print(f"Skipping TCP latency histogram/CDF: {path} is empty.")
        return

    ordered = sorted(samples)
    p50 = percentile(ordered, 0.50)
    p95 = percentile(ordered, 0.95)
    p99 = percentile(ordered, 0.99)

    # Histogram
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.hist(samples, bins=60)
    ax.axvline(p50, linestyle="--", label=f"p50 = {p50:.2f} µs")
    ax.axvline(p95, linestyle="--", label=f"p95 = {p95:.2f} µs")
    ax.axvline(p99, linestyle="--", label=f"p99 = {p99:.2f} µs")
    ax.set_xlabel("Round-trip order acknowledgement latency (µs)")
    ax.set_ylabel("Samples")
    ax.set_title("End-to-end TCP latency distribution")
    ax.legend()
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(FIGURES / "tcp_latency_histogram.png", dpi=180)
    plt.close(fig)

    # CDF
    n = len(ordered)
    cdf = [(i + 1) / n for i in range(n)]

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.plot(ordered, cdf)
    ax.axvline(p50, linestyle="--", label=f"p50 = {p50:.2f} µs")
    ax.axvline(p95, linestyle="--", label=f"p95 = {p95:.2f} µs")
    ax.axvline(p99, linestyle="--", label=f"p99 = {p99:.2f} µs")
    ax.set_xlabel("Round-trip order acknowledgement latency (µs)")
    ax.set_ylabel("Cumulative probability")
    ax.set_title("End-to-end TCP latency CDF")
    ax.set_ylim(0.0, 1.01)
    ax.legend()
    ax.grid(alpha=0.25)
    fig.tight_layout()
    fig.savefig(FIGURES / "tcp_latency_cdf.png", dpi=180)
    plt.close(fig)

    print(
        f"Loaded {len(samples):,} raw latency samples from "
        f"{path.relative_to(ROOT)}"
    )
    print(
        f"Raw-sample percentiles: "
        f"p50={p50:.3f} µs, "
        f"p95={p95:.3f} µs, "
        f"p99={p99:.3f} µs"
    )


def main():
    save_queue_throughput()
    save_queue_speedup()
    save_matching_throughput()
    save_latency_percentiles()
    save_memory_pool_allocations()
    save_latency_distribution()
    print(f"Figures written to: {FIGURES}")


if __name__ == "__main__":
    main()
