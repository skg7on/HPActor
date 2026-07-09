#!/usr/bin/env python3
"""Build native dependencies for the Python ABI3 wheel.

Downloads (via fetch_source.py) and builds platform-correct static
libraries for OpenSSL, Abseil, and protobuf into a single prefix
directory suitable for use with HPACTOR_WHEEL_DEPS_PREFIX.

Usage:
    build_native_deps.py --prefix PREFIX [--cache CACHE] [--build-dir DIR]
                         [--deployment-target VER] [--jobs N] [--keep-going]
"""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


def detect_target() -> str:
    """Return the build target triple for the current platform."""
    machine = platform.machine()
    system = platform.system()

    if system == "Darwin":
        if machine == "x86_64":
            return "darwin64-x86_64-cc"
        elif machine in ("arm64", "aarch64"):
            return "darwin64-arm64-cc"
        else:
            raise RuntimeError(f"Unsupported macOS architecture: {machine}")
    elif system == "Linux":
        if machine == "x86_64":
            return "linux-x86_64"
        elif machine in ("aarch64", "arm64"):
            return "linux-aarch64"
        else:
            raise RuntimeError(f"Unsupported Linux architecture: {machine}")
    else:
        raise RuntimeError(f"Unsupported platform: {system}")


def cmake_arch() -> str:
    """Return the CMake architecture string for the current platform."""
    machine = platform.machine()
    if machine in ("arm64", "aarch64"):
        return "arm64"
    return "x86_64"


def build_openssl(source_dir: Path, prefix: Path, target: str,
                  jobs: int) -> None:
    """Build OpenSSL as static libraries."""
    print(f"Building OpenSSL for {target} ...", file=sys.stderr)
    subprocess.run(
        [
            "perl", "Configure", target,
            "no-shared", "no-tests", "no-docs",
            f"--prefix={prefix}",
            f"--openssldir={prefix}/ssl",
        ],
        cwd=str(source_dir),
        check=True,
    )
    subprocess.run(
        ["make", f"-j{jobs}"],
        cwd=str(source_dir),
        check=True,
    )
    subprocess.run(
        ["make", "install_sw"],
        cwd=str(source_dir),
        check=True,
    )


def build_abseil(source_dir: Path, prefix: Path, jobs: int,
                 deployment_target: str, arch: str) -> None:
    """Build Abseil as a static library with CMake."""
    print("Building Abseil ...", file=sys.stderr)
    build_dir = source_dir / "build"
    build_dir.mkdir(exist_ok=True)

    cmake_args = [
        "cmake", "-S", str(source_dir), "-B", str(build_dir),
        "-GNinja",
        f"-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DABSL_BUILD_TESTING=OFF",
        "-DABSL_PROPAGATE_CXX_STD=ON",
        f"-DCMAKE_OSX_ARCHITECTURES={arch}",
    ]
    if deployment_target:
        cmake_args.append(
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}"
        )

    subprocess.run(cmake_args, check=True)
    subprocess.run(
        ["ninja", "-C", str(build_dir), f"-j{jobs}"],
        check=True,
    )
    subprocess.run(
        ["ninja", "-C", str(build_dir), "install"],
        check=True,
    )


def build_protobuf(source_dir: Path, prefix: Path, abseil_prefix: Path,
                   jobs: int, deployment_target: str, arch: str) -> None:
    """Build protobuf C++ as a static library with CMake."""
    print("Building protobuf ...", file=sys.stderr)
    build_dir = source_dir / "build"
    build_dir.mkdir(exist_ok=True)

    cmake_args = [
        "cmake", "-S", str(source_dir), "-B", str(build_dir),
        "-GNinja",
        f"-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=OFF",
        "-Dprotobuf_BUILD_TESTS=OFF",
        "-Dprotobuf_BUILD_SHARED_LIBS=OFF",
        "-Dprotobuf_ABSL_PROVIDER=package",
        f"-DCMAKE_PREFIX_PATH={abseil_prefix}",
        f"-DCMAKE_OSX_ARCHITECTURES={arch}",
    ]
    if deployment_target:
        cmake_args.append(
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}"
        )

    subprocess.run(cmake_args, check=True)
    subprocess.run(
        ["ninja", "-C", str(build_dir), f"-j{jobs}"],
        check=True,
    )
    subprocess.run(
        ["ninja", "-C", str(build_dir), "install"],
        check=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build native dependencies for hpactor wheel"
    )
    parser.add_argument(
        "--prefix", required=True,
        help="Install prefix for built libraries",
    )
    parser.add_argument(
        "--cache",
        default="build/wheel-deps/cache",
        help="Download cache directory",
    )
    parser.add_argument(
        "--build-dir",
        default="build/wheel-deps/build",
        help="Temporary build directory",
    )
    parser.add_argument(
        "--deployment-target",
        default="",
        help="macOS deployment target (e.g. 12.0)",
    )
    parser.add_argument(
        "--jobs", type=int,
        default=os.cpu_count() or 4,
        help="Parallel build jobs",
    )
    parser.add_argument(
        "--keep-going", action="store_true",
        help="Continue after build failures",
    )
    args = parser.parse_args()

    prefix = Path(args.prefix)
    cache_dir = Path(args.cache)
    build_root = Path(args.build_dir)
    target = detect_target()
    arch = cmake_arch()

    # Resolve lock file relative to this script
    script_dir = Path(__file__).resolve().parent
    lock_path = script_dir / "native-deps.lock.json"
    fetch_script = script_dir / "fetch_source.py"

    if not lock_path.exists():
        print(f"Lock file not found: {lock_path}", file=sys.stderr)
        sys.exit(1)

    # Step 1: fetch and extract sources
    extract_dir = build_root / "src"
    extract_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            sys.executable, str(fetch_script),
            "--lock", str(lock_path),
            "--cache", str(cache_dir),
            "--extract-to", str(extract_dir),
            "--keep-going",
        ],
        check=True,
    )

    lock = json.loads(lock_path.read_text())
    prefix.mkdir(parents=True, exist_ok=True)

    # Step 2: build OpenSSL
    openssl_dir = extract_dir / lock["openssl"]["source_dir"]
    if openssl_dir.exists():
        build_openssl(openssl_dir, prefix, target, args.jobs)

    # Step 3: build Abseil
    abseil_dir = extract_dir / lock["abseil"]["source_dir"]
    if abseil_dir.exists():
        build_abseil(abseil_dir, prefix, args.jobs,
                     args.deployment_target, arch)

    # Step 4: build protobuf
    protobuf_dir = extract_dir / lock["protobuf"]["source_dir"]
    if protobuf_dir.exists():
        build_protobuf(protobuf_dir, prefix, prefix,
                       args.jobs, args.deployment_target, arch)

    # Write build manifest
    manifest = {
        "target": target,
        "architecture": arch,
        "deployment_target": args.deployment_target or None,
        "prefix": str(prefix),
        "dependencies": {
            name: entry["version"] for name, entry in lock.items()
        },
    }
    manifest_path = build_root / "dependency-build.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))
    print(f"\nDependency build complete: {manifest_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
