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
from urllib.parse import urlparse, urlunparse


# ── Redirect handler — reject non-HTTPS redirects ───────────────────────

class _HttpsOnlyRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Reject redirects to non-HTTPS schemes (http://, file://, ftp://)."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        scheme = urlparse(newurl).scheme
        if scheme != "https":
            raise urllib.request.URLError(
                f"Refusing to follow redirect to non-HTTPS URL: {newurl}"
            )
        return urllib.request.HTTPRedirectHandler.redirect_request(
            self, req, fp, code, msg, headers, newurl
        )


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

        # Validate source_dir — must not contain path traversal
        sd = source_dir.replace("\\", "/")
        if ".." in sd.split("/") or sd.startswith("/"):
            print(
                f"ERROR: {name}: source_dir contains path traversal: "
                f"{source_dir}", file=sys.stderr,
            )
            sys.exit(1)

        # Only allow HTTPS
        if urlparse(url).scheme != "https":
            print(f"ERROR: {name}: URL must be HTTPS: {url}", file=sys.stderr)
            sys.exit(1)

        # Derive safe archive name from URL path (no query/fragment)
        archive_name = os.path.basename(urlparse(url).path)
        if not archive_name or ".." in archive_name:
            print(
                f"ERROR: {name}: unsafe archive name from URL: {url}",
                file=sys.stderr,
            )
            sys.exit(1)
        cache_path = (args.cache / archive_name).resolve()
        if not str(cache_path).startswith(str(args.cache.resolve()) + os.sep):
            print(
                f"ERROR: {name}: archive path escapes cache: {cache_path}",
                file=sys.stderr,
            )
            sys.exit(1)
        stamp_path = args.cache / f"{archive_name}.sha256"

        # Validate extract_dir is a safe descendant of args.extract
        extract_dir = (args.extract / source_dir).resolve()
        extract_root = args.extract.resolve()
        if not str(extract_dir).startswith(str(extract_root) + os.sep):
            print(
                f"ERROR: {name}: source_dir escapes extraction root: "
                f"{source_dir}", file=sys.stderr,
            )
            sys.exit(1)

        # Already verified?
        if stamp_path.exists():
            cached_digest = stamp_path.read_text().strip()
            if cached_digest == expected and cache_path.exists():
                print(f"[OK] {name}: cached {archive_name} sha256={expected[:12]}...")
                # Extract if needed
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

        # Download — use secure opener that rejects non-HTTPS redirects
        print(f"[FETCH] {name}: {url}")
        secure_opener = urllib.request.build_opener(
            _HttpsOnlyRedirectHandler()
        )
        try:
            secure_opener.retrieve(url, cache_path)
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

        # Atomic extraction: extract to temp, then rename into place
        if extract_dir.exists():
            shutil.rmtree(extract_dir)
        print(f"     extracting to {extract_dir}")
        _extract(cache_path, extract_dir)
        # Verify extraction produced meaningful content
        extracted = list(extract_dir.iterdir())
        if not extracted:
            print(f"ERROR: {name}: extraction produced empty directory",
                  file=sys.stderr)
            sys.exit(1)
        print(f"     {len(extracted)} top-level entries")

    print("All dependencies fetched and verified.")


def _extract(archive: Path, dest: Path) -> None:
    """Extract *archive* to *dest*, flattening a single top-level dir.

    Uses :func:`shutil.unpack_archive` for robust extraction, then moves
    the contents of the (expected single) top-level directory into *dest*.
    """
    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        # Use tarfile directly with filter='data' for path-traversal protection
        with tarfile.open(archive, "r:*") as tf:
            tf.extractall(tmp, filter="data")
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
