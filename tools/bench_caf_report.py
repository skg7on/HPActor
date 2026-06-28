#!/usr/bin/env python3
"""
CAF Performance Benchmark Report Generator

Runs all 18_bench_caf scenarios, collects JSON output, computes summary
statistics, and renders a human-readable performance report.

Usage:
    python3 tools/bench_caf_report.py [--binary PATH] [--preset smoke|nightly|paper-scale] [--output report.md]
"""

import argparse
import json
import os
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

# ── Configuration ───────────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = REPO_ROOT / "build" / "apps" / "bench_caf" / "18_bench_caf"

SCENARIOS = {
    # Phase 1
    "actor-creation": {
        "phase": 1,
        "category": "actor-lifecycle",
        "description": "Recursive actor spawn, fan-in, teardown, allocator pressure",
        "key_metric": "actors_created",
    },
    "mailbox-n1": {
        "phase": 1,
        "category": "mailbox-contention",
        "description": "Many producers sending to one receiver mailbox",
        "key_metric": "throughput_msgps",
    },
    "mixed-case": {
        "phase": 1,
        "category": "mixed-workload",
        "description": "Ring token passing, actor lifecycle churn, CPU work",
        "key_metric": "token_hops",
    },
    # Phase 2
    "traffic-one-to-one": {
        "phase": 2,
        "category": "traffic-distribution",
        "description": "Baseline 1:1 send/receive overhead",
        "key_metric": "throughput_msgps",
    },
    "traffic-one-to-n": {
        "phase": 2,
        "category": "traffic-distribution",
        "description": "Fanout to N receivers, scheduler wakeup spread",
        "key_metric": "throughput_msgps",
    },
    "traffic-n-to-n-random": {
        "phase": 2,
        "category": "traffic-distribution",
        "description": "N senders to M receivers, LCG routing",
        "key_metric": "throughput_msgps",
    },
    "traffic-ring": {
        "phase": 2,
        "category": "traffic-distribution",
        "description": "Token passing around a ring, steady-state latency",
        "key_metric": "token_hops",
    },
    "traffic-pipeline": {
        "phase": 2,
        "category": "traffic-distribution",
        "description": "Staged pipeline, handoff and batching effects",
        "key_metric": "throughput_msgps",
    },
    "traffic-zipf": {
        "phase": 2,
        "category": "traffic-distribution",
        "description": "Skewed Zipf receiver selection, overload concentration",
        "key_metric": "throughput_msgps",
    },
    "traffic-bursty": {
        "phase": 2,
        "category": "traffic-distribution",
        "description": "Batched bursts with idle gaps, queue recovery",
        "key_metric": "throughput_msgps",
    },
    # Phase 3
    "message-creation": {
        "phase": 3,
        "category": "microbenchmark",
        "description": "TypedMessage and StreamBuffer construction cost",
        "key_metric": "throughput_msgps",
    },
    "dispatch-match": {
        "phase": 3,
        "category": "microbenchmark",
        "description": "TypeTag dispatch through EventBasedActor behavior",
        "key_metric": "throughput_msgps",
    },
    "serialization": {
        "phase": 3,
        "category": "microbenchmark",
        "description": "Protobuf encode/decode throughput",
        "key_metric": "throughput_msgps",
    },
    "mandelbrot": {
        "phase": 3,
        "category": "cpu-scheduling",
        "description": "CPU-bound Mandelbrot computation, scheduler fairness",
        "key_metric": "cpu_tasks_completed",
    },
    "scheduling-mix": {
        "phase": 3,
        "category": "mixed-workload",
        "description": "Concurrent spawn bursts, CPU tasks, and message rings",
        "key_metric": "actors_created",
    },
    "distributed-ping": {
        "phase": 3,
        "category": "messaging",
        "description": "Cross-group ping/pong with reply routing",
        "key_metric": "throughput_msgps",
    },
}

PRESETS = ["smoke", "nightly", "paper-scale", "stress"]
DEFAULT_PRESET = "smoke"
# Per-scenario timeout — nightly/paper-scale sweep 6–8 message sizes
# with cooperative batch senders; large presets need more wall time.
TIMEOUT_PER_SCENARIO = 600  # seconds


# ── Helpers ──────────────────────────────────────────────────────

def run_scenario(binary: Path, scenario: str, preset: str,
                 trials: int = 1, timeout: int = TIMEOUT_PER_SCENARIO
                 ) -> dict | None:
    """Run a single benchmark scenario and return parsed JSON."""
    cmd = [
        str(binary),
        "--scenario", scenario,
        "--preset", preset,
        "--format", "json",
        "--trials", str(trials),
    ]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=REPO_ROOT,
        )
        if result.returncode != 0:
            print(f"  [ERROR] exit={result.returncode}: {result.stderr.strip()[:120]}",
                  file=sys.stderr)
            return None
        return json.loads(result.stdout)
    except subprocess.TimeoutExpired:
        print(f"  [TIMEOUT] after {timeout}s", file=sys.stderr)
        return None
    except json.JSONDecodeError as e:
        print(f"  [JSON-ERROR] {e}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"  [ERROR] {e}", file=sys.stderr)
        return None


def extract_metrics(data: dict) -> dict:
    """Extract key metrics from a JSON benchmark report."""
    trials = data.get("trials", [])
    if not trials:
        return {}

    # Aggregate across trials
    completed = [t for t in trials if t.get("completed")]
    if not completed:
        return {
            "completed_trials": 0,
            "total_trials": len(trials),
            "runtime_ms_mean": 0,
            "runtime_ms_min": 0,
            "runtime_ms_max": 0,
            "throughput_mean": 0,
            "throughput_p95": 0,
            "total_sent": 0,
            "total_received": 0,
            "total_dropped": 0,
            "peak_rss_bytes": 0,
        }

    def _mean(vals):
        return sum(vals) / len(vals) if vals else 0.0

    def _p95(vals):
        if not vals:
            return 0
        svals = sorted(vals)
        idx = int(len(svals) * 0.95)
        return svals[min(idx, len(svals) - 1)]

    runtimes = [t["runtime_ms"] for t in completed]
    throughputs = [t.get("throughput_msgps", 0) for t in completed]
    sent = [t.get("total_sent", 0) for t in completed]
    received = [t.get("total_received", 0) for t in completed]
    dropped = [t.get("total_dropped", 0) for t in completed]
    peak_rss = [t.get("peak_rss_bytes", 0) for t in completed]

    return {
        "completed_trials": len(completed),
        "total_trials": len(trials),
        "runtime_ms_mean": _mean(runtimes),
        "runtime_ms_min": min(runtimes) if runtimes else 0,
        "runtime_ms_max": max(runtimes) if runtimes else 0,
        "throughput_mean": _mean(throughputs),
        "throughput_p95": _p95(throughputs),
        "total_sent": sum(sent),
        "total_received": sum(received),
        "total_dropped": sum(dropped),
        "peak_rss_bytes": max(peak_rss) if peak_rss else 0,
    }


def format_bytes(b: int | float) -> str:
    """Format bytes to human-readable string."""
    if b >= 1_073_741_824:
        return f"{b / 1_073_741_824:.2f} GiB"
    if b >= 1_048_576:
        return f"{b / 1_048_576:.2f} MiB"
    if b >= 1024:
        return f"{b / 1024:.1f} KiB"
    return f"{int(b)} B"


def format_rate(rate: float) -> str:
    """Format a throughput rate."""
    if rate >= 1_000_000:
        return f"{rate / 1_000_000:.2f} M msg/s"
    if rate >= 1_000:
        return f"{rate / 1_000:.1f} K msg/s"
    return f"{rate:.0f} msg/s"


def format_time(ms: float) -> str:
    """Format milliseconds to human-readable."""
    if ms >= 60_000:
        return f"{ms / 60_000:.1f} min"
    if ms >= 1_000:
        return f"{ms / 1_000:.2f} s"
    return f"{ms:.0f} ms"


# ── Report Generation ────────────────────────────────────────────

def generate_report(results: dict, preset: str, binary: str) -> str:
    """Generate a human-readable markdown performance report."""
    lines = []
    lines.append("# HPActor CAF Benchmark Performance Report")
    lines.append("")
    lines.append(f"**Preset:** `{preset}`  ")
    lines.append(f"**Binary:** `{binary}`  ")
    lines.append(f"**Date:** {time.strftime('%Y-%m-%d %H:%M:%S')}  ")
    lines.append(f"**Machine:** {os.uname().sysname} {os.uname().machine}  ")
    lines.append("")

    # ── Overall Summary ──────────────────────────────────────────
    lines.append("## Overall Summary")
    lines.append("")

    total = len(results)
    passed = sum(1 for r in results.values()
                 if r and r.get("completed_trials", 0) > 0)
    failed = total - passed
    lines.append(f"| | Count |")
    lines.append(f"|---|------:|")
    lines.append(f"| Scenarios run | {total} |")
    lines.append(f"| Completed successfully | {passed} |")
    lines.append(f"| Failed / timed out | {failed} |")
    lines.append("")

    # ── Summary by Phase ─────────────────────────────────────────
    lines.append("## Results by Phase")
    lines.append("")

    for phase in [1, 2, 3]:
        phase_scenarios = {k: v for k, v in results.items()
                          if SCENARIOS[k]["phase"] == phase}
        lines.append(f"### Phase {phase}")
        lines.append("")
        lines.append("| Scenario | Trials | Runtime | Throughput | "
                      "Sent | Received | Dropped | Peak RSS |")
        lines.append("|---|--:|---:|---:|---:|---:|---:|---:|")

        for name, meta in sorted(phase_scenarios.items(),
                                  key=lambda x: SCENARIOS[x[0]]["category"]):
            if meta is None or meta.get("completed_trials", 0) == 0:
                lines.append(f"| `{name}` | ❌ FAILED | — | — | "
                              "— | — | — | — |")
                continue

            m = meta
            runtime = format_time(m["runtime_ms_mean"])
            tp = format_rate(m["throughput_mean"])
            rss = format_bytes(m["peak_rss_bytes"])
            lines.append(
                f"| `{name}` | {m['completed_trials']}/{m['total_trials']} "
                f"| {runtime} | {tp} | {m['total_sent']:,} "
                f"| {m['total_received']:,} | {m['total_dropped']:,} "
                f"| {rss} |"
            )
        lines.append("")

    # ── Category Breakdown ───────────────────────────────────────
    lines.append("## Category Breakdown")
    lines.append("")

    categories = defaultdict(list)
    for name, meta in results.items():
        cat = SCENARIOS[name]["category"]
        categories[cat].append((name, meta))

    for cat in sorted(categories):
        cat_results = [(n, m) for n, m in categories[cat]
                       if m and m.get("completed_trials", 0) > 0]
        if not cat_results:
            continue

        avg_tp = sum(m["throughput_mean"] for _, m in cat_results) / len(cat_results)
        avg_rt = sum(m["runtime_ms_mean"] for _, m in cat_results) / len(cat_results)
        lines.append(f"### {cat.replace('-', ' ').title()}")
        lines.append(f"- {len(cat_results)} scenario(s)")
        lines.append(f"- Average throughput: {format_rate(avg_tp)}")
        lines.append(f"- Average runtime: {format_time(avg_rt)}")
        lines.append("")

    # ── Top Performers ───────────────────────────────────────────
    lines.append("## Top Performers")
    lines.append("")

    ranked = sorted(
        [(n, m) for n, m in results.items()
         if m and m.get("completed_trials", 0) > 0],
        key=lambda x: x[1]["throughput_mean"],
        reverse=True,
    )

    lines.append("### Highest Throughput")
    lines.append("")
    lines.append("| Rank | Scenario | Phase | Throughput | Category |")
    lines.append("|--:|---|---|---:|----|")
    for i, (name, meta) in enumerate(ranked[:5], 1):
        info = SCENARIOS[name]
        lines.append(f"| {i} | `{name}` | {info['phase']} "
                      f"| {format_rate(meta['throughput_mean'])} "
                      f"| {info['category']} |")
    lines.append("")

    lines.append("### Lowest Runtime")
    lines.append("")
    ranked_rt = sorted(
        [(n, m) for n, m in results.items()
         if m and m.get("completed_trials", 0) > 0],
        key=lambda x: x[1]["runtime_ms_mean"],
    )
    lines.append("| Rank | Scenario | Phase | Runtime | Category |")
    lines.append("|--:|---|---|---:|----|")
    for i, (name, meta) in enumerate(ranked_rt[:5], 1):
        info = SCENARIOS[name]
        lines.append(f"| {i} | `{name}` | {info['phase']} "
                      f"| {format_time(meta['runtime_ms_mean'])} "
                      f"| {info['category']} |")
    lines.append("")

    # ── Message Delivery Reliability ─────────────────────────────
    lines.append("## Message Delivery Reliability")
    lines.append("")

    delivery_scenarios = [(n, m) for n, m in results.items()
                          if m and m.get("total_sent", 0) > 0]
    lines.append("| Scenario | Sent | Received | Drop Rate |")
    lines.append("|---|---|---:|---:|")
    for name, meta in sorted(delivery_scenarios,
                              key=lambda x: x[1].get("total_dropped", 0),
                              reverse=True):
        sent = meta["total_sent"]
        recv = meta["total_received"]
        dropped = meta["total_dropped"]
        rate = f"{dropped / sent * 100:.3f}%" if sent > 0 else "N/A"
        lines.append(f"| `{name}` | {sent:,} | {recv:,} | {rate} |")
    lines.append("")

    # ── Memory Footprint ─────────────────────────────────────────
    lines.append("## Memory Footprint (Peak RSS)")
    lines.append("")

    ranked_mem = sorted(
        [(n, m) for n, m in results.items()
         if m and m.get("peak_rss_bytes", 0) > 0],
        key=lambda x: x[1]["peak_rss_bytes"],
        reverse=True,
    )
    lines.append("| Rank | Scenario | Phase | Peak RSS | Category |")
    lines.append("|--:|---|---|---:|----|")
    for i, (name, meta) in enumerate(ranked_mem[:5], 1):
        info = SCENARIOS[name]
        lines.append(f"| {i} | `{name}` | {info['phase']} "
                      f"| {format_bytes(meta['peak_rss_bytes'])} "
                      f"| {info['category']} |")
    lines.append("")

    # ── Warnings / Anomalies ─────────────────────────────────────
    warnings = []
    for name, meta in results.items():
        if meta is None:
            warnings.append(f"- ❌ `{name}`: failed to complete (binary error or timeout)")
        elif meta.get("completed_trials", 0) == 0:
            warnings.append(f"- ⚠️  `{name}`: 0/{meta['total_trials']} trials completed")
        elif meta.get("total_dropped", 0) > 0:
            warnings.append(
                f"- ⚠️  `{name}`: {meta['total_dropped']:,} messages dropped "
                f"({meta['total_dropped'] / max(meta['total_sent'], 1) * 100:.2f}%)"
            )

    if warnings:
        lines.append("## Warnings / Anomalies")
        lines.append("")
        for w in warnings:
            lines.append(w)
        lines.append("")

    # ── Scenario Reference ───────────────────────────────────────
    lines.append("## Scenario Reference")
    lines.append("")
    lines.append("| # | Scenario | Phase | Category | Key Metric |")
    lines.append("|--:|---|---|---|----|")
    for i, (name, info) in enumerate(sorted(SCENARIOS.items(),
                                              key=lambda x: (x[1]["phase"],
                                                            x[1]["category"])), 1):
        lines.append(f"| {i} | `{name}` | {info['phase']} "
                      f"| {info['category']} | {info['key_metric']} |")
    lines.append("")

    lines.append("---")
    lines.append(f"*Report generated by `tools/bench_caf_report.py` "
                  f"at {time.strftime('%Y-%m-%d %H:%M:%S')}*")
    lines.append("")

    return "\n".join(lines)


# ── Main ─────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="CAF Performance Benchmark Report Generator"
    )
    parser.add_argument(
        "--binary",
        type=Path,
        default=DEFAULT_BINARY,
        help=f"Path to 18_bench_caf binary (default: {DEFAULT_BINARY})",
    )
    parser.add_argument(
        "--preset",
        choices=PRESETS,
        default=DEFAULT_PRESET,
        help=f"Benchmark preset to run (default: {DEFAULT_PRESET})",
    )
    parser.add_argument(
        "--trials",
        type=int,
        default=1,
        help="Number of trials per scenario (default: 1)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Write report to file (default: stdout)",
    )
    parser.add_argument(
        "--scenarios",
        nargs="*",
        help="Specific scenarios to run (default: all)",
    )
    parser.add_argument(
        "--json-output",
        type=Path,
        help="Write raw JSON results to file",
    )
    args = parser.parse_args()

    # Validate binary
    binary = args.binary
    if not binary.exists():
        # Try build/ directory relative to repo root
        binary = REPO_ROOT / "build" / "apps" / "bench_caf" / "18_bench_caf"
    if not binary.exists():
        print(f"Error: binary not found at {binary}. "
              "Build with: cmake -S . -B build -GNinja -DENABLE_APPS=ON && ninja -C build",
              file=sys.stderr)
        sys.exit(1)

    # Select scenarios
    if args.scenarios:
        selected = {s: SCENARIOS[s] for s in args.scenarios if s in SCENARIOS}
        unknown = [s for s in args.scenarios if s not in SCENARIOS]
        if unknown:
            print(f"Unknown scenarios: {unknown}", file=sys.stderr)
            print(f"Valid: {', '.join(sorted(SCENARIOS))}", file=sys.stderr)
            sys.exit(1)
    else:
        selected = dict(SCENARIOS)

    # Run benchmarks
    total = len(selected)
    print(f"Running {total} scenario(s) with preset '{args.preset}' "
          f"({args.trials} trial(s) each)...\n")
    print(f"{'#':>3}  {'Scenario':<28} {'Status':<12} {'Runtime':>10} {'Throughput':>14}")
    print(f"{'-'*3}  {'-'*28} {'-'*12} {'-'*10} {'-'*14}")

    raw_results = {}
    results = {}
    start_all = time.time()

    # Run mixed-case last: its sustained multi-ring workload can trigger
    # macOS thermal / resource throttling that degrades subsequent benchmarks.
    for i, (name, info) in enumerate(sorted(selected.items(),
                                              key=lambda x: (x[0] == "mixed-case",
                                                            x[1]["phase"],
                                                            x[1]["category"])), 1):
        sys.stdout.write(f"{i:>3}  {name:<28} ")
        sys.stdout.flush()

        data = run_scenario(binary, name, args.preset, args.trials)
        raw_results[name] = data
        metrics = extract_metrics(data) if data else None
        results[name] = metrics

        if metrics and metrics.get("completed_trials", 0) > 0:
            rt = format_time(metrics["runtime_ms_mean"])
            tp = format_rate(metrics["throughput_mean"])
            print(f"{'OK':<12} {rt:>10} {tp:>14}")
        elif metrics:
            print(f"{'FAILED':<12} {'—':>10} {'—':>14}")
        else:
            print(f"{'ERROR':<12} {'—':>10} {'—':>14}")

    elapsed = time.time() - start_all
    print(f"\nCompleted in {format_time(elapsed * 1000)}\n")

    # Save raw JSON if requested
    if args.json_output:
        with open(args.json_output, "w") as f:
            json.dump(raw_results, f, indent=2, default=str)
        print(f"Raw results saved to {args.json_output}")

    # Generate report
    report = generate_report(results, args.preset, str(binary))

    if args.output:
        with open(args.output, "w") as f:
            f.write(report)
        print(f"Report written to {args.output}")
    else:
        print(report)


if __name__ == "__main__":
    main()
