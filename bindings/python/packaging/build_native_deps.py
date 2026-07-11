#!/usr/bin/env python3
"""Build platform-correct static native dependencies for the hpactor wheel.

Builds OpenSSL, Abseil, and protobuf from verified sources into a single
prefix with PIC static libraries and a matching protoc.  No network access
is performed — sources must already be fetched and verified by fetch_source.py.

Usage:
    python3 build_native_deps.py --prefix build/wheel-deps/prefix \\
        --source build/wheel-deps/src \\
        --build-dir build/wheel-deps/build \\
        --deployment-target 12.0 --jobs 8
"""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


# ── Target detection ──────────────────────────────────────────────────────

def _detect_target(deployment_target: str | None,
                   arch_override: str | None = None) -> dict:
    """Return {arch, os, openssl_target, cmake_arch} for the current machine."""
    machine = arch_override or platform.machine()
    system = platform.system()

    if system == "Linux":
        if machine == "x86_64":
            return {
                "arch": "x86_64",
                "os": "linux",
                "openssl_target": "linux-x86_64",
                "cmake_arch": "x86_64",
            }
        elif machine in ("aarch64", "arm64"):
            return {
                "arch": "aarch64",
                "os": "linux",
                "openssl_target": "linux-aarch64",
                "cmake_arch": "aarch64",
            }
    elif system == "Darwin":
        if machine == "x86_64":
            return {
                "arch": "x86_64",
                "os": "macos",
                "openssl_target": "darwin64-x86_64-cc",
                "cmake_arch": "x86_64",
            }
        elif machine in ("arm64", "aarch64"):
            return {
                "arch": "arm64",
                "os": "macos",
                "openssl_target": "darwin64-arm64-cc",
                "cmake_arch": "arm64",
            }

    raise RuntimeError(f"Unsupported platform: {system} {machine}")


# ── Builders ──────────────────────────────────────────────────────────────

def _build_openssl(prefix: Path, source_dir: Path, build_dir: Path,
                   target: dict, jobs: int) -> None:
    src = source_dir / "openssl-3.5.5"

    # OpenSSL must be configured and built in its source tree
    subprocess.run(
        [
            "./Configure",
            target["openssl_target"],
            "no-shared",
            "no-tests",
            "--prefix", str(prefix),
            "--openssldir", str(prefix / "ssl"),
        ],
        cwd=str(src),
        check=True,
    )
    subprocess.run(["make", f"-j{jobs}"], cwd=str(src), check=True)
    subprocess.run(["make", "install_sw"], cwd=str(src), check=True)
    print(f"[OK] OpenSSL 3.5.5 installed to {prefix}")


def _build_abseil(prefix: Path, source_dir: Path, build_dir: Path,
                  target: dict, deployment_target: str | None,
                  jobs: int) -> None:
    src = source_dir / "abseil-cpp-20260107.1"
    work = build_dir / "abseil"
    work.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        "cmake", "-S", str(src), "-B", str(work), "-GNinja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=" + str(prefix),
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DABSL_BUILD_TESTING=OFF",
        "-DABSL_PROPAGATE_CXX_STD=ON",
        f"-DCMAKE_CXX_STANDARD=20",
    ]
    if target["os"] == "macos" and deployment_target:
        cmake_args.append(
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}"
        )
        cmake_args.append(f"-DCMAKE_OSX_ARCHITECTURES={target['cmake_arch']}")

    subprocess.run(cmake_args, check=True)
    subprocess.run(["ninja", "-C", str(work), f"-j{jobs}"], check=True)
    subprocess.run(
        ["cmake", "--install", str(work), "--prefix", str(prefix)],
        check=True,
    )
    print(f"[OK] Abseil 20260107.1 installed to {prefix}")


def _build_protobuf(prefix: Path, source_dir: Path, build_dir: Path,
                    target: dict, deployment_target: str | None,
                    jobs: int) -> None:
    src = source_dir / "protobuf-35.0"
    work = build_dir / "protobuf"
    work.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        "cmake", "-S", str(src), "-B", str(work), "-GNinja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=" + str(prefix),
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=OFF",
        "-Dprotobuf_BUILD_TESTS=OFF",
        "-Dprotobuf_BUILD_SHARED_LIBS=OFF",
        "-Dprotobuf_ABSL_PROVIDER=package",
        f"-DCMAKE_PREFIX_PATH={prefix}",
        f"-DCMAKE_CXX_STANDARD=20",
    ]
    if target["os"] == "macos" and deployment_target:
        cmake_args.append(
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}"
        )
        cmake_args.append(f"-DCMAKE_OSX_ARCHITECTURES={target['cmake_arch']}")

    subprocess.run(cmake_args, check=True)
    subprocess.run(["ninja", "-C", str(work), f"-j{jobs}"], check=True)
    subprocess.run(
        ["cmake", "--install", str(work), "--prefix", str(prefix)],
        check=True,
    )
    print(f"[OK] protobuf 35.0 installed to {prefix}")


# ── Main ──────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build native wheel dependencies"
    )
    parser.add_argument(
        "--prefix", required=True, type=Path,
        help="Installation prefix for built libraries",
    )
    parser.add_argument(
        "--source", required=True, type=Path,
        help="Directory with extracted sources",
    )
    parser.add_argument(
        "--build-dir", required=True, type=Path,
        help="Scratch build directory",
    )
    parser.add_argument(
        "--deployment-target", type=str, default=None,
        help="macOS deployment target (e.g. 12.0)",
    )
    parser.add_argument(
        "--arch", type=str, default=None,
        help="Target architecture override (x86_64, aarch64, arm64)",
    )
    parser.add_argument(
        "--jobs", type=int, default=os.cpu_count() or 4,
        help="Parallel build jobs",
    )
    args = parser.parse_args()

    # Resolve paths so subprocess calls with changed cwd work correctly
    prefix = args.prefix.resolve()
    source = args.source.resolve()
    build_dir = args.build_dir.resolve()

    target = _detect_target(args.deployment_target, args.arch)
    print(f"Platform: {target['os']} {target['arch']}")

    prefix.mkdir(parents=True, exist_ok=True)
    build_dir.mkdir(parents=True, exist_ok=True)

    _build_openssl(prefix, source, build_dir,
                   target, args.jobs)
    _build_abseil(prefix, source, build_dir,
                  target, args.deployment_target, args.jobs)
    _build_protobuf(prefix, source, build_dir,
                    target, args.deployment_target, args.jobs)

    # Write dependency-build.json
    report = {
        "openssl": "3.5.5",
        "abseil": "20260107.1",
        "protobuf": "35.0",
        "arch": target["arch"],
        "os": target["os"],
        "deployment_target": args.deployment_target,
    }
    report_path = prefix / "dependency-build.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Build report written to {report_path}")


if __name__ == "__main__":
    main()
