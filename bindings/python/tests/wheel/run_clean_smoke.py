#!/usr/bin/env python3
"""Run hpactor wheel smoke tests in an isolated environment.

Creates a fresh venv, installs the wheel plus runtime dependencies,
and runs the wheel test suite from a temporary directory outside the
source checkout.

Usage:
    run_clean_smoke.py --python PYTHON --wheel WHEEL [--protobuf VERSION]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run hpactor wheel smoke tests in isolation"
    )
    parser.add_argument(
        "--python", required=True,
        help="Path to the CPython interpreter to use",
    )
    parser.add_argument(
        "--wheel", required=True,
        help="Path to the hpactor .whl file",
    )
    parser.add_argument(
        "--protobuf",
        default="protobuf>=7.35.0,<8",
        help="protobuf version specifier (default: protobuf>=7.35.0,<8)",
    )
    parser.add_argument(
        "--test-dir",
        help="Directory containing wheel test files "
             "(default: auto-detect relative to this script)",
    )
    args = parser.parse_args()

    wheel = Path(args.wheel)
    if not wheel.exists():
        print(f"Wheel not found: {wheel}", file=sys.stderr)
        sys.exit(1)

    python = args.python
    # Verify the Python interpreter works
    version_check = subprocess.run(
        [python, "--version"], capture_output=True, text=True
    )
    if version_check.returncode != 0:
        print(f"Python interpreter not working: {python}", file=sys.stderr)
        sys.exit(1)
    print(f"Python: {version_check.stdout.strip()}", file=sys.stderr)

    # Find test directory
    if args.test_dir:
        test_dir = Path(args.test_dir)
    else:
        # Auto-detect relative to this script
        test_dir = Path(__file__).resolve().parent
    if not test_dir.exists():
        print(f"Test directory not found: {test_dir}", file=sys.stderr)
        sys.exit(1)

    # Create a temporary working directory outside the checkout
    work_dir = Path(tempfile.mkdtemp(prefix="hpactor-smoke-"))
    print(f"Working directory: {work_dir}", file=sys.stderr)

    try:
        # Create venv
        venv_dir = work_dir / "venv"
        subprocess.run(
            [python, "-m", "venv", str(venv_dir)],
            check=True,
        )

        pip = str(venv_dir / "bin" / "pip")
        test_python = str(venv_dir / "bin" / "python")

        # Upgrade pip
        subprocess.run(
            [pip, "install", "--upgrade", "pip"],
            check=True,
            capture_output=True,
        )

        # Install wheel and dependencies
        subprocess.run(
            [pip, "install", str(wheel), args.protobuf],
            check=True,
        )

        # Verify import works from venv
        subprocess.run(
            [test_python, "-I", "-c", "import hpactor; print(hpactor.__version__)"],
            check=True,
        )

        # Copy test files to work dir (so we don't import from checkout)
        test_target = work_dir / "tests"
        shutil.copytree(test_dir, test_target,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

        # Run tests from the work directory with PYTHONPATH cleared
        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        result = subprocess.run(
            [test_python, "-I", "-m", "unittest", "discover",
             "-s", str(test_target), "-p", "test_*.py", "-v"],
            cwd=str(work_dir),
            env=env,
        )
        print(f"\nSmoke tests {'passed' if result.returncode == 0 else 'failed'}")
        sys.exit(result.returncode)

    finally:
        # Clean up
        if os.environ.get("HPACTOR_KEEP_SMOKE_DIR"):
            print(f"Keeping work dir: {work_dir}", file=sys.stderr)
        else:
            shutil.rmtree(work_dir, ignore_errors=True)
            print("Cleaned up work directory.", file=sys.stderr)


if __name__ == "__main__":
    main()
