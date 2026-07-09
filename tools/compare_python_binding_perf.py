#!/usr/bin/env python3
"""Compare two hpactor Python binding performance results.

Rejects comparisons across different hardware, validates schema, and
checks for regressions exceeding a configurable threshold (default 20%).

Usage:
    compare_python_binding_perf.py --reference REF.json --candidate CAND.json
                                   [--threshold 0.20] [--report REPORT.md]
"""

import argparse
import json
import sys
from pathlib import Path


def load_result(path: Path) -> dict:
    """Load and return a benchmark result JSON."""
    return json.loads(path.read_text())


def validate_schema(result: dict) -> list[str]:
    """Basic schema validation. Returns list of error messages."""
    errors = []
    required_top = ["version", "platform", "architecture", "cpu_model",
                    "runner_fingerprint", "wheel_sha256", "scenarios"]
    for key in required_top:
        if key not in result:
            errors.append(f"Missing required top-level key: {key}")

    if "scenarios" in result:
        required_scenarios = [
            "throughput", "dispatch_wait_ns", "handler_latency_ns",
            "ask_p50_ns", "ask_p95_ns", "ask_p99_ns",
        ]
        for key in required_scenarios:
            if key not in result["scenarios"]:
                errors.append(f"Missing required scenario: {key}")
    return errors


def compare(reference: dict, candidate: dict,
            threshold: float) -> dict:
    """Compare candidate against reference. Returns comparison report."""
    # Fingerprint check
    ref_fp = reference.get("runner_fingerprint", "")
    cand_fp = candidate.get("runner_fingerprint", "")
    if ref_fp != cand_fp:
        raise ValueError(
            f"Runner fingerprint mismatch:\n"
            f"  reference: {ref_fp}\n"
            f"  candidate: {cand_fp}"
        )

    report = {
        "passed": True,
        "reference_version": reference.get("version"),
        "candidate_version": candidate.get("version"),
        "reference_sha256": reference.get("wheel_sha256"),
        "candidate_sha256": candidate.get("wheel_sha256"),
        "threshold": threshold,
        "metrics": {},
    }

    # Throughput: (ref - cand) / ref > threshold is a regression
    ref_scenarios = reference.get("scenarios", {})
    cand_scenarios = candidate.get("scenarios", {})

    for name in ["throughput"]:
        ref_val = ref_scenarios.get(name, {}).get("value", 0)
        cand_val = cand_scenarios.get(name, {}).get("value", 0)
        if ref_val > 0:
            regression = (ref_val - cand_val) / ref_val
            is_regression = regression > threshold
            report["metrics"][name] = {
                "reference": ref_val,
                "candidate": cand_val,
                "regression": regression,
                "is_regression": is_regression,
            }
            if is_regression:
                report["passed"] = False

    # Latency: (cand - ref) / ref > threshold is a regression
    for name in ["dispatch_wait_ns", "handler_latency_ns",
                 "ask_p50_ns", "ask_p95_ns", "ask_p99_ns"]:
        ref_val = ref_scenarios.get(name, {}).get("value", 0)
        cand_val = cand_scenarios.get(name, {}).get("value", 0)
        if ref_val > 0:
            regression = (cand_val - ref_val) / ref_val
            is_regression = regression > threshold
            report["metrics"][name] = {
                "reference": ref_val,
                "candidate": cand_val,
                "regression": regression,
                "is_regression": is_regression,
            }
            if is_regression:
                report["passed"] = False

    return report


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare hpactor Python binding performance results"
    )
    parser.add_argument("--reference", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--threshold", type=float, default=0.20)
    parser.add_argument("--report", default=None)
    args = parser.parse_args()

    ref_path = Path(args.reference)
    cand_path = Path(args.candidate)

    if not ref_path.exists():
        print(f"Reference file not found: {ref_path}", file=sys.stderr)
        sys.exit(1)
    if not cand_path.exists():
        print(f"Candidate file not found: {cand_path}", file=sys.stderr)
        sys.exit(1)

    reference = load_result(ref_path)
    candidate = load_result(cand_path)

    # Validate schemas
    ref_errors = validate_schema(reference)
    cand_errors = validate_schema(candidate)
    all_errors = ref_errors + cand_errors
    if all_errors:
        for err in all_errors:
            print(f"Schema error: {err}", file=sys.stderr)
        sys.exit(1)

    try:
        report = compare(reference, candidate, args.threshold)
    except ValueError as e:
        print(f"Comparison error: {e}", file=sys.stderr)
        sys.exit(1)

    # Write report
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=2))

    if report["passed"]:
        print("Performance comparison PASSED", file=sys.stderr)
    else:
        print("Performance comparison FAILED — regressions detected:",
              file=sys.stderr)
        for name, metric in report["metrics"].items():
            if metric.get("is_regression"):
                print(
                    f"  {name}: {metric['regression']:.1%} regression "
                    f"(ref={metric['reference']}, cand={metric['candidate']})",
                    file=sys.stderr,
                )
        sys.exit(1)


if __name__ == "__main__":
    main()
