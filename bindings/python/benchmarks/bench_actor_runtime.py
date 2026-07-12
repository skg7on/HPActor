#!/usr/bin/env python3
"""Python binding actor runtime benchmarks.

Measures empty-handler throughput, dispatch wait, handler latency,
and ask/reply end-to-end p50/p95/p99 on an installed wheel.

NOTE: This is a measurement harness scaffold.  Real measurement requires
a full hpactor runtime inside a clean venv with the installed wheel.
The current implementation records the harness overhead as a smoke test;
actual actor benchmarks will be wired in a follow-up PR once the wheel
build pipeline stabilizes.

Usage:
    python bench_actor_runtime.py --wheel hpactor-*.whl \\
        --output perf.json --warmup 10000 --iterations 100000
"""

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path


def _p50(values: list[float]) -> float:
    return _nearest_rank(values, 0.50)


def _p95(values: list[float]) -> float:
    return _nearest_rank(values, 0.95)


def _p99(values: list[float]) -> float:
    return _nearest_rank(values, 0.99)


def _nearest_rank(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    idx = max(0, min(len(s) - 1, int(round(percentile * (len(s) - 1)))))
    return s[idx]


def main() -> None:
    parser = argparse.ArgumentParser(description="hpactor Python benchmark")
    parser.add_argument("--wheel", required=True, type=Path,
                        help="Path to installed wheel")
    parser.add_argument("--output", required=True, type=Path,
                        help="JSON output path")
    parser.add_argument("--warmup", type=int, default=10000)
    parser.add_argument("--iterations", type=int, default=100000)
    parser.add_argument("--payload-bytes", type=int, default=64)
    args = parser.parse_args()

    # Collect environment fingerprint
    fingerprint = {
        "arch": platform.machine(),
        "os": platform.system(),
        "cpu_model": platform.processor() or "unknown",
        "python_version": platform.python_version(),
        "logical_cpus": os.cpu_count() or 0,
    }

    # Compute wheel SHA256 for reproducibility tracking
    wheel_sha256 = "TBD"
    if args.wheel.exists():
        h = hashlib.sha256()
        with open(args.wheel, "rb") as fh:
            while True:
                chunk = fh.read(1 << 20)
                if not chunk:
                    break
                h.update(chunk)
        wheel_sha256 = h.hexdigest()

    result = {
        "wheel_sha256": wheel_sha256,
        **fingerprint,
        "parameters": {
            "warmup": args.warmup,
            "iterations": args.iterations,
            "payload_bytes": args.payload_bytes,
        },
        "status": "scaffold",
        "note": (
            "Measurement harness scaffold — real actor benchmarks "
            "require a full hpactor runtime inside a clean venv with the "
            "installed wheel.  Wire in a follow-up PR."
        ),
        "trials": 0,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Benchmark scaffold written to {args.output}")


if __name__ == "__main__":
    main()
