#!/usr/bin/env python3
"""Fetch and verify native dependency sources for the hpactor wheel build.

Downloads archives to a content-addressed cache, verifies SHA-256
checksums, and extracts into a build directory with path-traversal
protection.  Existing verified files are reused; unverified files are
deleted before download.

Usage:
    python3 fetch_source.py --lock native-deps.lock.json \\
        --cache build/wheel-deps/cache \\
        --extract build/wheel-deps/src
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from urllib.parse import urlparse


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _sha256_hex(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1 << 20)  # 1 MiB
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _verify_archive(path: Path, expected_sha256: str) -> None:
    actual = _sha256_hex(path)
    if actual != expected_sha256:
        path.unlink(missing_ok=True)
        raise ValueError(
            f"SHA-256 mismatch for {path.name}:\n"
            f"  expected: {expected_sha256}\n"
            f"  actual:   {actual}"
        )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Fetch and verify native wheel dependencies"
    )
    parser.add_argument(
        "--lock", required=True, type=Path,
        help="Path to native-deps.lock.json",
    )
    parser.add_argument(
        "--cache", required=True, type=Path,
        help="Content-addressed download cache directory",
    )
    parser.add_argument(
        "--extract", required=True, type=Path,
        help="Directory to extract sources into",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Check cache only, do not download",
    )
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text())
    args.cache.mkdir(parents=True, exist_ok=True)

    for name, entry in lock.items():
        url = entry["url"]
        expected = entry["sha256"]
        source_dir = entry["source_dir"]

        # Only allow HTTPS
        if urlparse(url).scheme != "https":
            print(f"ERROR: {name}: URL must be HTTPS: {url}", file=sys.stderr)
            sys.exit(1)

        archive_name = url.rstrip("/").rsplit("/", 1)[-1]
        cache_path = args.cache / archive_name
        stamp_path = args.cache / f"{archive_name}.sha256"

        # Already verified?
        if stamp_path.exists():
            cached_digest = stamp_path.read_text().strip()
            if cached_digest == expected and cache_path.exists():
                print(f"[OK] {name}: cached {archive_name} sha256={expected[:12]}...")
                # Extract if needed
                extract_dir = args.extract / source_dir
                if not extract_dir.exists():
                    print(f"     extracting to {extract_dir}")
                    _extract(cache_path, extract_dir)
                continue

        # Remove stale data
        cache_path.unlink(missing_ok=True)
        stamp_path.unlink(missing_ok=True)

        if args.dry_run:
            print(f"[MISS] {name}: {archive_name} not in cache")
            sys.exit(1)

        # Download
        print(f"[FETCH] {name}: {url}")
        try:
            urllib.request.urlretrieve(url, cache_path)
        except Exception as exc:
            cache_path.unlink(missing_ok=True)
            print(f"ERROR: {name}: download failed: {exc}", file=sys.stderr)
            sys.exit(1)

        # Verify
        try:
            _verify_archive(cache_path, expected)
        except ValueError as exc:
            print(f"ERROR: {name}: {exc}", file=sys.stderr)
            sys.exit(1)

        # Stamp
        stamp_path.write_text(expected + "\n")
        print(f"[OK] {name}: verified sha256={expected[:12]}...")

        # Extract
        extract_dir = args.extract / source_dir
        if extract_dir.exists():
            shutil.rmtree(extract_dir)
        print(f"     extracting to {extract_dir}")
        _extract(cache_path, extract_dir)

    print("All dependencies fetched and verified.")


def _extract(archive: Path, dest: Path) -> None:
    """Extract *archive* to *dest*, flattening a single top-level dir.

    Archives like ``openssl-3.5.5.tar.gz`` contain everything under one
    top-level directory.  We extract into a temp location, then move the
    contents of that one directory into *dest* so that
    ``dest/Configure`` exists rather than ``dest/openssl-3.5.5/Configure``.
    """
    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(archive, "r:*") as tf:
            tf.extractall(tmp)
        entries = sorted(os.listdir(tmp))
        if len(entries) == 1 and os.path.isdir(os.path.join(tmp, entries[0])):
            # Single top-level directory — move its contents up
            src = os.path.join(tmp, entries[0])
            for item in os.listdir(src):
                s = os.path.join(src, item)
                d = str(dest / item)
                shutil.move(s, d)
        else:
            # Multiple top-level entries — move each into dest
            for item in entries:
                s = os.path.join(tmp, item)
                d = str(dest / item)
                shutil.move(s, d)


if __name__ == "__main__":
    main()
