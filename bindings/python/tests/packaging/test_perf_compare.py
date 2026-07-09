"""Tests for the performance comparison and regression gate."""

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path


class PerfCompareTest(unittest.TestCase):
    """Verify the performance comparator logic."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())

    def test_comparator_script_exists(self) -> None:
        script = self.root / "tools" / "compare_python_binding_perf.py"
        self.assertTrue(script.exists(), f"{script} not found")

    def test_comparator_rejects_different_fingerprint(self) -> None:
        """Comparison across different hardware must be rejected."""
        script = self.root / "tools" / "compare_python_binding_perf.py"

        ref = {
            "version": "0.1.0",
            "platform": "darwin",
            "architecture": "arm64",
            "cpu_model": "Apple M1",
            "runner_fingerprint": "darwin-arm64-Apple M1",
            "wheel_sha256": "a" * 64,
            "logical_cpus": 8,
            "scenarios": {
                "throughput": {"value": 1000, "unit": "ops/sec",
                               "samples": 100, "warmup": 10, "runs": 5},
                "dispatch_wait_ns": {"value": 100, "unit": "ns",
                                     "samples": 100},
                "handler_latency_ns": {"value": 50, "unit": "ns",
                                       "samples": 100},
                "ask_p50_ns": {"value": 1000, "unit": "ns",
                               "samples": 100},
                "ask_p95_ns": {"value": 2000, "unit": "ns",
                               "samples": 100},
                "ask_p99_ns": {"value": 3000, "unit": "ns",
                               "samples": 100},
            },
        }
        cand = dict(ref)
        cand["runner_fingerprint"] = "linux-x86_64-Intel"

        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            ref_path = Path(tmp) / "ref.json"
            cand_path = Path(tmp) / "cand.json"
            ref_path.write_text(json.dumps(ref))
            cand_path.write_text(json.dumps(cand))

            result = subprocess.run(
                [sys.executable, str(script),
                 "--reference", str(ref_path),
                 "--candidate", str(cand_path)],
                capture_output=True, text=True,
            )
            self.assertNotEqual(
                result.returncode, 0,
                "Comparator must reject different fingerprints",
            )

    def test_comparator_detects_regression(self) -> None:
        """A >20% throughput drop must be flagged."""
        script = self.root / "tools" / "compare_python_binding_perf.py"

        ref = {
            "version": "0.1.0",
            "platform": "darwin",
            "architecture": "arm64",
            "cpu_model": "Apple M1",
            "runner_fingerprint": "darwin-arm64-Apple M1",
            "wheel_sha256": "a" * 64,
            "logical_cpus": 8,
            "scenarios": {
                "throughput": {"value": 1000, "unit": "ops/sec",
                               "samples": 100, "warmup": 10, "runs": 5},
                "dispatch_wait_ns": {"value": 100, "unit": "ns",
                                     "samples": 100},
                "handler_latency_ns": {"value": 50, "unit": "ns",
                                       "samples": 100},
                "ask_p50_ns": {"value": 1000, "unit": "ns",
                               "samples": 100},
                "ask_p95_ns": {"value": 2000, "unit": "ns",
                               "samples": 100},
                "ask_p99_ns": {"value": 3000, "unit": "ns",
                               "samples": 100},
            },
        }
        # 25% regression in throughput
        cand = json.loads(json.dumps(ref))
        cand["scenarios"]["throughput"]["value"] = 750
        cand["scenarios"]["ask_p95_ns"]["value"] = 2500

        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            ref_path = Path(tmp) / "ref.json"
            cand_path = Path(tmp) / "cand.json"
            ref_path.write_text(json.dumps(ref))
            cand_path.write_text(json.dumps(cand))

            result = subprocess.run(
                [sys.executable, str(script),
                 "--reference", str(ref_path),
                 "--candidate", str(cand_path)],
                capture_output=True, text=True,
            )
            self.assertNotEqual(
                result.returncode, 0,
                "Comparator must detect >20% throughput regression",
            )

    def test_bench_schema_exists(self) -> None:
        schema = (
            self.root / "bindings" / "python" / "benchmarks" / "schema.json"
        )
        self.assertTrue(schema.exists(), f"{schema} not found")

    def test_bench_script_exists(self) -> None:
        script = (
            self.root / "bindings" / "python" / "benchmarks"
            / "bench_actor_runtime.py"
        )
        self.assertTrue(script.exists(), f"{script} not found")
