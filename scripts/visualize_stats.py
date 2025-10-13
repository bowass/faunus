#!/usr/bin/env python3
"""Visualize Faunus per-thread metrics exported by `kv_test`.

This utility expects the JSON artifacts generated in the `thread_stats/` directory.
It prints a brief textual digest and, when `matplotlib` is available, emits PNG
figures that highlight throughput and latency trends per operation.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional

# Optional plotting dependency
try:
    import matplotlib.pyplot as plt  # type: ignore
except Exception:  # pragma: no cover - matplotlib is optional
    plt = None


@dataclass
class OperationStats:
    name: str
    successes: int = 0
    failures: int = 0
    avg_latency_us: Optional[float] = None
    min_latency_us: Optional[float] = None
    max_latency_us: Optional[float] = None
    total_latency_us: float = 0.0


@dataclass
class ThreadMetrics:
    compute_server: int
    thread: int
    attempted: int
    succeeded: int
    operations: Dict[str, OperationStats] = field(default_factory=dict)

    @staticmethod
    def from_json(data: Dict[str, object]) -> "ThreadMetrics":
        operations: Dict[str, OperationStats] = {}
        raw_ops = data.get("operations", {})
        for name, entry in raw_ops.items():
            operations[name] = OperationStats(
                name=name,
                successes=int(entry.get("successes", 0)),
                failures=int(entry.get("failures", 0)),
                avg_latency_us=_maybe_float(entry.get("avg_latency_us")),
                min_latency_us=_maybe_float(entry.get("min_latency_us")),
                max_latency_us=_maybe_float(entry.get("max_latency_us")),
                total_latency_us=float(entry.get("total_latency_us", 0.0)),
            )
        return ThreadMetrics(
            compute_server=int(data.get("compute_server", 0)),
            thread=int(data.get("thread", 0)),
            attempted=int(data.get("attempted", 0)),
            succeeded=int(data.get("succeeded", 0)),
            operations=operations,
        )


def _maybe_float(value) -> Optional[float]:
    if value is None or (isinstance(value, str) and value.lower() == "null"):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def discover_metrics(stats_dir: Path) -> tuple[Dict[str, object], List[ThreadMetrics]]:
    summary_path = stats_dir / "summary.json"
    if not summary_path.exists():
        raise FileNotFoundError(f"Could not find summary file at {summary_path}")

    with summary_path.open("r", encoding="utf-8") as fh:
        summary = json.load(fh)

    thread_metrics: List[ThreadMetrics] = []
    for path in sorted(stats_dir.glob("cs_*_thread_*.json")):
        with path.open("r", encoding="utf-8") as fh:
            data = json.load(fh)
        thread_metrics.append(ThreadMetrics.from_json(data))

    if not thread_metrics:
        raise FileNotFoundError(
            f"No thread metrics found under {stats_dir}. Expected files named 'cs_<id>_thread_<id>.json'."
        )

    return summary, thread_metrics


def print_digest(summary: Dict[str, object], threads: Iterable[ThreadMetrics]) -> None:
    print("=== Global Summary ===")
    print(f"Total attempted: {summary.get('total_attempted', 0)}")
    print(f"Total succeeded: {summary.get('total_succeeded', 0)}")

    elapsed = summary.get("elapsed_sec")
    if isinstance(elapsed, (int, float)):
        print(f"Elapsed runtime (s): {_fmt_optional(elapsed)}")

    op_counts: Dict[str, int] = summary.get("operation_counts", {})  # type: ignore[assignment]
    if op_counts:
        print("Operation counts:")
        for name, count in op_counts.items():
            print(f"  {name:<8} {count}")

    per_op_latency: Dict[str, Dict[str, object]] = summary.get("per_operation_latency", {})  # type: ignore[assignment]
    if per_op_latency:
        print("Latency (µs):")
        for name, entry in per_op_latency.items():
            avg = entry.get("avg_latency_us")
            min_latency = entry.get("min_latency_us")
            max_latency = entry.get("max_latency_us")
            print(
                f"  {name:<8} avg={_fmt_optional(avg)} min={_fmt_optional(min_latency)} max={_fmt_optional(max_latency)}"
            )

    throughput = summary.get("throughput")
    if isinstance(throughput, dict):
        window = throughput.get("window_sec")
        if isinstance(window, (int, float)):
            print(f"Throughput window (s): {_fmt_optional(window)}")
        attempted_tp = throughput.get("attempted_ops_per_sec")
        succeeded_tp = throughput.get("succeeded_ops_per_sec")
        if attempted_tp is not None or succeeded_tp is not None:
            print(
                "Overall throughput (ops/s): "
                f"attempted={_fmt_optional(attempted_tp)} succeeded={_fmt_optional(succeeded_tp)}"
            )
        per_operation = throughput.get("per_operation")
        if isinstance(per_operation, dict) and per_operation:
            print("Per-operation throughput (ops/s):")
            for name, values in per_operation.items():
                if isinstance(values, dict):
                    attempted = _fmt_optional(values.get("attempted_ops_per_sec"))
                    succeeded = _fmt_optional(values.get("succeeded_ops_per_sec"))
                    print(f"  {name:<8} attempted={attempted} succeeded={succeeded}")

    op_success_rates: Dict[str, List[float]] = {}
    for tm in threads:
        for name, op in tm.operations.items():
            total = op.successes + op.failures
            if total == 0:
                continue
            op_success_rates.setdefault(name, []).append(op.successes / total)

    if op_success_rates:
        print("Success rate by operation (mean ± std dev):")
        for name, rates in op_success_rates.items():
            mean = statistics.mean(rates)
            stdev = statistics.pstdev(rates) if len(rates) > 1 else 0.0
            print(f"  {name:<8} {mean:.3%} ± {stdev:.3%}")


def _fmt_optional(value: object) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "—"
    try:
        return f"{float(value):.3f}"
    except (TypeError, ValueError):
        return "—"


def generate_plots(
    summary: Dict[str, object],
    threads: List[ThreadMetrics],
    output_prefix: Path,
) -> None:
    if plt is None:
        print("matplotlib not available; skipping plot generation.")
        return

    output_prefix.parent.mkdir(parents=True, exist_ok=True)

    # Plot 1: Operation totals (successes only)
    op_counts: Dict[str, int] = summary.get("operation_counts", {})  # type: ignore[assignment]
    names = list(op_counts.keys())
    counts = [op_counts[name] for name in names]

    fig, ax = plt.subplots(figsize=(8, 4.5))
    bars = ax.bar(names, counts, color="#1377ff")
    ax.set_title("Operation volume (successes)")
    ax.set_ylabel("Operations")
    ax.bar_label(bars, padding=4, fmt="%d")
    fig.tight_layout()
    fig.savefig(Path(f"{output_prefix}_operation_counts.png"), dpi=160)
    plt.close(fig)

    # Plot 2: Per-thread average latency per operation
    latency_data: Dict[str, List[float]] = {}
    for tm in threads:
        for name, op in tm.operations.items():
            if op.avg_latency_us is not None:
                latency_data.setdefault(name, []).append(op.avg_latency_us)

    if latency_data:
        fig, ax = plt.subplots(figsize=(8, 4.5))
        labels = list(latency_data.keys())
        data = [latency_data[label] for label in labels]
        ax.boxplot(data, labels=labels, showfliers=False)
        ax.set_title("Per-thread average latency (µs)")
        ax.set_ylabel("Latency (µs)")
        fig.tight_layout()
        fig.savefig(Path(f"{output_prefix}_latency_boxplot.png"), dpi=160)
        plt.close(fig)

    # Plot 3: Success rate per thread (stacked by operation)
    thread_labels: List[str] = []
    success_by_op: Dict[str, List[float]] = {}
    for tm in threads:
        label = f"cs{tm.compute_server}-t{tm.thread}"
        thread_labels.append(label)
        for name, op in tm.operations.items():
            total = op.successes + op.failures
            ratio = (op.successes / total) if total else 0.0
            success_by_op.setdefault(name, []).append(ratio)

    if thread_labels and success_by_op:
        fig, ax = plt.subplots(figsize=(max(10, len(thread_labels) * 0.6), 5))
        bottom = [0.0] * len(thread_labels)
        for name, ratios in success_by_op.items():
            if len(ratios) < len(thread_labels):
                ratios = ratios + [0.0] * (len(thread_labels) - len(ratios))
            ax.bar(thread_labels, ratios, bottom=bottom, label=name)
            bottom = [b + r for b, r in zip(bottom, ratios)]

        ax.set_ylim(0, 1)
        ax.set_ylabel("Success ratio")
        ax.set_title("Thread-level success ratio by operation")
        ax.legend()
        fig.autofmt_xdate(rotation=45, ha="right")
        fig.tight_layout()
        fig.savefig(Path(f"{output_prefix}_success_ratio.png"), dpi=160)
        plt.close(fig)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "stats_dir",
        nargs="?",
        type=Path,
        default=Path("thread_stats"),
        help="Directory containing per-thread JSON stats (default: %(default)s)",
    )
    parser.add_argument(
        "-o",
        "--output-prefix",
        type=Path,
        default=Path("thread_stats/faunus"),
        help="Filename prefix for generated plots (default: %(default)s)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    summary, thread_metrics = discover_metrics(args.stats_dir)
    print_digest(summary, thread_metrics)
    generate_plots(summary, thread_metrics, args.output_prefix)


if __name__ == "__main__":
    main()
