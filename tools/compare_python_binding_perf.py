#!/usr/bin/env python3
"""Compare two hpactor benchmark results and report regressions.

Usage:
    python compare_python_binding_perf.py --reference ref.json \\
        --candidate cand.json --threshold 0.20 --output report.json
"""

import argparse
import json
import sys
from pathlib import Path


def _fingerprint_key(result: dict) -> str:
    return f"{result.get('arch')}-{result.get('os')}-{result.get('cpu_model')}"


def main() -> None:
    parser = argparse.ArgumentParser(description="Compare hpactor benchmarks")
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--threshold", type=float, default=0.20)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    ref = json.loads(args.reference.read_text())
    cand = json.loads(args.candidate.read_text())

    # Same-runner enforcement
    if _fingerprint_key(ref) != _fingerprint_key(cand):
        print("ERROR: runner fingerprint mismatch — refusing to compare "
              "results from different hardware", file=sys.stderr)
        sys.exit(2)

    metrics = {
        "throughput_ops_per_sec": ("lower", "throughput"),
        "p50_ns": ("higher", "latency"),
        "p95_ns": ("higher", "latency"),
        "p99_ns": ("higher", "latency"),
    }

    regressions = []
    passed = True

    for metric, (direction, category) in metrics.items():
        r = ref.get(metric, 0)
        c = cand.get(metric, 0)
        if r == 0:
            continue

        if direction == "higher":
            ratio = (c - r) / r
        else:
            ratio = (r - c) / r

        if ratio > args.threshold:
            regressions.append({
                "metric": metric,
                "category": category,
                "reference": r,
                "candidate": c,
                "ratio": round(ratio, 4),
                "threshold": args.threshold,
            })
            passed = False

    report = {
        "passed": passed,
        "reference_fingerprint": _fingerprint_key(ref),
        "candidate_fingerprint": _fingerprint_key(cand),
        "threshold": args.threshold,
        "regressions": regressions,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")

    if not passed:
        print(f"FAIL: {len(regressions)} regression(s) exceed "
              f"{args.threshold * 100:.0f}% threshold")
        for r in regressions:
            print(f"  {r['metric']}: {r['ratio']*100:.1f}% "
                  f"(ref={r['reference']:.1f}, cand={r['candidate']:.1f})")
        sys.exit(1)

    print("OK: no performance regressions")


if __name__ == "__main__":
    main()
