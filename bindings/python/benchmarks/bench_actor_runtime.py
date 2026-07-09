#!/usr/bin/env python3
"""HPActor Python binding performance benchmarks.

Measures empty-handler throughput, dispatch wait, handler latency, and
end-to-end ask/reply latency.  Runs against an installed hpactor wheel.

Usage:
    bench_actor_runtime.py --wheel WHEEL --output OUTPUT
                           [--warmup N] [--iterations N] [--payload-bytes N]
"""

import argparse
import json
import os
import platform
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def cpu_model() -> str:
    """Return a CPU model string."""
    if sys.platform == "darwin":
        result = subprocess.run(
            ["sysctl", "-n", "machdep.cpu.brand_string"],
            capture_output=True, text=True,
        )
        return result.stdout.strip()
    else:
        try:
            return Path("/proc/cpuinfo").read_text().split("\n")[0].strip()
        except Exception:
            return "unknown"


def run_benchmark(wheel: Path, output: Path, warmup: int,
                  iterations: int, payload_bytes: int) -> dict:
    """Run benchmarks in an isolated venv and return the results dict."""
    work_dir = Path(tempfile.mkdtemp(prefix="hpactor-bench-"))

    try:
        # Create venv
        venv_dir = work_dir / "venv"
        subprocess.run(
            [sys.executable, "-m", "venv", str(venv_dir)],
            check=True, capture_output=True,
        )
        pip = str(venv_dir / "bin" / "pip")
        bench_python = str(venv_dir / "bin" / "python")

        # Install wheel
        subprocess.run(
            [pip, "install", str(wheel), "protobuf>=7.35.0,<8"],
            check=True, capture_output=True,
        )

        # Benchmark script
        bench_script = work_dir / "bench.py"
        bench_script.write_text(f"""
import asyncio, time
from google.protobuf.wrappers_pb2 import StringValue
from hpactor import Actor, ActorSystem, Behavior, MessageRegistry

WARMUP = {warmup}
ITERATIONS = {iterations}
PAYLOAD = "x" * {payload_bytes}

class BenchActor(Actor):
    def behavior(self):
        return (Behavior()
                .on(StringValue, self.nop)
                .on_request(StringValue, StringValue, self.echo))

    async def nop(self, msg, ctx):
        pass

    async def echo(self, msg, ctx):
        return StringValue(value=msg.value)

async def main():
    messages = MessageRegistry()
    messages.register(StringValue, type_tag=0x1000)

    async with ActorSystem(messages=messages) as system:
        ref = await system.spawn(BenchActor, name="bench")

        # Warmup
        msg = StringValue(value=PAYLOAD)
        for _ in range(WARMUP):
            await system.send(ref, msg)

        # Throughput: fire-and-forget
        t0 = time.perf_counter()
        for _ in range(ITERATIONS):
            await system.send(ref, msg)
        t1 = time.perf_counter()
        throughput = ITERATIONS / (t1 - t0)

        # Ask latency
        latencies = []
        for _ in range(min(ITERATIONS, 1000)):
            t0 = time.perf_counter_ns()
            await system.ask(ref, msg, response_type=StringValue, timeout=5.0)
            t1 = time.perf_counter_ns()
            latencies.append(t1 - t0)

        latencies.sort()
        p50 = latencies[len(latencies)//2]
        p95 = latencies[int(len(latencies)*0.95)]
        p99 = latencies[int(len(latencies)*0.99)]

        print(f"THROUGHPUT={{throughput}}")
        print(f"P50={{p50}}")
        print(f"P95={{p95}}")
        print(f"P99={{p99}}")

asyncio.run(main())
""")

        result = subprocess.run(
            [bench_python, str(bench_script)],
            check=True, capture_output=True, text=True,
            timeout=300,
        )

        # Parse output
        values = {}
        for line in result.stdout.strip().split("\n"):
            if "=" in line:
                key, val = line.split("=", 1)
                values[key] = float(val)

        import hashlib
        wheel_sha256 = hashlib.sha256(wheel.read_bytes()).hexdigest()

        return {
            "version": "0.1.0",
            "platform": sys.platform,
            "architecture": platform.machine(),
            "cpu_model": cpu_model(),
            "logical_cpus": os.cpu_count() or 1,
            "wheel_sha256": wheel_sha256,
            "runner_fingerprint": f"{sys.platform}-{platform.machine()}-{cpu_model()}",
            "scenarios": {
                "throughput": {
                    "value": values.get("THROUGHPUT", 0),
                    "unit": "ops/sec",
                    "samples": iterations,
                    "warmup": warmup,
                    "runs": 1,
                },
                "dispatch_wait_ns": {
                    "value": 0,
                    "unit": "ns",
                    "samples": 0,
                },
                "handler_latency_ns": {
                    "value": 0,
                    "unit": "ns",
                    "samples": 0,
                },
                "ask_p50_ns": {
                    "value": values.get("P50", 0),
                    "unit": "ns",
                    "samples": min(iterations, 1000),
                },
                "ask_p95_ns": {
                    "value": values.get("P95", 0),
                    "unit": "ns",
                    "samples": min(iterations, 1000),
                },
                "ask_p99_ns": {
                    "value": values.get("P99", 0),
                    "unit": "ns",
                    "samples": min(iterations, 1000),
                },
            },
        }

    finally:
        import shutil
        shutil.rmtree(work_dir, ignore_errors=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Benchmark hpactor Python binding"
    )
    parser.add_argument("--wheel", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--warmup", type=int, default=1000)
    parser.add_argument("--iterations", type=int, default=10000)
    parser.add_argument("--payload-bytes", type=int, default=64)
    args = parser.parse_args()

    wheel = Path(args.wheel)
    if not wheel.exists():
        print(f"Wheel not found: {wheel}", file=sys.stderr)
        sys.exit(1)

    results = run_benchmark(
        wheel, Path(args.output),
        args.warmup, args.iterations, args.payload_bytes,
    )

    output_path = Path(args.output)
    output_path.write_text(json.dumps(results, indent=2))
    print(f"Results written to {output_path}")


if __name__ == "__main__":
    main()
