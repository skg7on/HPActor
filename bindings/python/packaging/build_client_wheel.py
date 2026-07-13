#!/usr/bin/env python3
# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Reproducible CMake-free universal (py3-none-any) client-only wheel build.

Usage:
    python3 build_client_wheel.py --output wheelhouse/client
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build universal client-only wheel"
    )
    parser.add_argument(
        "--output", default="wheelhouse/client",
        help="Output directory for the built wheel",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[3]
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Remove any stale wheels in the output directory
    for stale in output_dir.glob("hpactor-*-py3-none-any.whl"):
        stale.unlink()

    # Build with scikit-build-core in pure-Python mode:
    # - wheel.cmake=false: skip CMake/native build
    # - wheel.py-api=py3: universal Python tag
    command = [
        sys.executable, "-m", "build", "--wheel",
        "-Cwheel.cmake=false",
        "-Cwheel.platlib=false",
        "-Cwheel.py-api=py3",
        "--outdir", str(output_dir),
    ]

    result = subprocess.run(
        command, cwd=repo_root, capture_output=False
    )
    if result.returncode != 0:
        print("Client wheel build failed", file=sys.stderr)
        sys.exit(1)

    wheels = list(output_dir.glob("hpactor-*-py3-none-any.whl"))
    if not wheels:
        print("No py3-none-any wheel produced", file=sys.stderr)
        sys.exit(1)

    print(f"Built: {wheels[0].name}")
    print("Done.")


if __name__ == "__main__":
    main()
