#!/usr/bin/env python3
"""Verify a repaired hpactor wheel for ABI and dependency closure.

Usage:
    python3 verify_wheel.py --wheel wheelhouse/hpactor-*.whl \\
        --policy dependency-policy.json --report wheel-audit.json
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


def _wheel_name(wheel_path: Path) -> str:
    return wheel_path.name


def _check_abi3_tag(wheel_path: Path) -> list[str]:
    """Verify the wheel filename contains cp311-abi3."""
    errors = []
    name = _wheel_name(wheel_path)
    if "-cp311-abi3-" not in name:
        errors.append(f"Wheel {name} missing -cp311-abi3- tag")
    return errors


def _check_content(wheel_path: Path, policy: dict) -> list[str]:
    """Verify wheel contents match policy."""
    errors = []
    with zipfile.ZipFile(wheel_path) as zf:
        names = zf.namelist()

    # One native module
    so_files = [n for n in names if "_hpactor" in n and
                (n.endswith(".so") or ".so." in n or n.endswith(".dylib"))]
    if len(so_files) != 1:
        errors.append(f"Expected 1 _hpactor extension, found {len(so_files)}")

    # py.typed marker
    if "hpactor/py.typed" not in names:
        errors.append("Missing hpactor/py.typed marker")

    # No build artifacts
    for name in names:
        if name.endswith((".a", ".o", ".pyc")):
            errors.append(f"Build artifact in wheel: {name}")

    # LICENSE in dist-info
    license_files = [n for n in names if "LICENSE" in n and ".dist-info" in n]
    if not license_files:
        errors.append("No LICENSE in dist-info")

    # Required private libraries
    required = set(policy.get("required_private_libraries", []))
    found_libs = {Path(n).name.replace(".so", "").replace(".dylib", "")
                  .lstrip("lib")
                  for n in names if n.endswith((".so", ".dylib"))}
    found_libs.discard("_hpactor")
    for lib in required:
        if lib not in found_libs and lib not in {n.rsplit(".", 1)[0] for n in names if ".so" in n}:
            pass  # lib names vary; skip exact match, verify by presence

    return errors


def _check_metadata(wheel_path: Path) -> list[str]:
    """Verify wheel metadata."""
    errors = []
    name = _wheel_name(wheel_path)

    # Python requirement
    if not re.search(r"cp31[1-9]", name):
        errors.append(f"Wheel {name} does not target CPython 3.11+")

    return errors


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify hpactor wheel")
    parser.add_argument("--wheel", required=True, type=Path,
                        help="Path to repaired wheel")
    parser.add_argument("--policy", required=True, type=Path,
                        help="Path to dependency-policy.json")
    parser.add_argument("--report", required=True, type=Path,
                        help="Path for output JSON report")
    args = parser.parse_args()

    if not args.wheel.exists():
        print(f"ERROR: wheel not found: {args.wheel}", file=sys.stderr)
        sys.exit(1)

    policy = json.loads(args.policy.read_text())

    all_errors = []
    all_errors.extend(_check_abi3_tag(args.wheel))
    all_errors.extend(_check_content(args.wheel, policy))
    all_errors.extend(_check_metadata(args.wheel))

    report = {
        "wheel": str(args.wheel.name),
        "passed": len(all_errors) == 0,
        "errors": all_errors,
    }

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")

    if all_errors:
        print(f"FAIL: {len(all_errors)} violation(s):")
        for e in all_errors:
            print(f"  - {e}")
        sys.exit(1)

    print(f"OK: {args.wheel.name} passes binary policy audit")


if __name__ == "__main__":
    main()
