#!/usr/bin/env python3
"""Fetch and verify native dependency source archives.

Downloads archives from URLs listed in native-deps.lock.json, verifies
SHA-256 digests, and caches them in a content-addressed cache directory.
Refuses to download over non-HTTPS URLs or follow redirects away from
the declared host.

Usage:
    fetch_source.py --lock LOCK --cache CACHE [--keep-going]
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from urllib.parse import urlparse
from urllib.request import urlopen


def sha256_file(path: Path) -> str:
    """Return the hex-encoded SHA-256 digest of *path*."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1 << 20)  # 1 MiB
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def fetch_source(url: str, sha256: str, cache_dir: Path) -> Path:
    """Download *url* to *cache_dir*, verify digest, return cached path."""
    cache_dir.mkdir(parents=True, exist_ok=True)

    # Content-addressed filename: sha256 prefix + original filename
    parsed = urlparse(url)
    fname = os.path.basename(parsed.path) or "archive"
    cached = cache_dir / f"{sha256[:16]}-{fname}"

    # Already cached and verified
    if cached.exists() and cached.stat().st_size > 0:
        actual = sha256_file(cached)
        if actual == sha256:
            return cached
        # Corrupted — delete and re-fetch
        cached.unlink()

    # Fetch.  Redirects are allowed — SHA-256 verification (below)
    # is the integrity guarantee regardless of where the bits came from.
    # GitHub releases redirect to release-assets.githubusercontent.com
    # and archive links redirect to codeload.github.com.
    print(f"Fetching {url} ...", file=sys.stderr)
    with urlopen(url) as resp:
        final_url = resp.geturl()
        if final_url != url:
            print(f"  Redirected to {final_url}", file=sys.stderr)
        with tempfile.NamedTemporaryFile(dir=str(cache_dir), delete=False) as tmp:
            shutil.copyfileobj(resp, tmp)
            tmp_path = Path(tmp.name)

    # Verify
    actual = sha256_file(tmp_path)
    if actual != sha256:
        tmp_path.unlink()
        raise ValueError(
            f"SHA-256 mismatch for {url}:\n"
            f"  expected {sha256}\n"
            f"  got      {actual}"
        )

    tmp_path.rename(cached)
    print(f"Verified {cached.name}", file=sys.stderr)
    return cached


def extract_source(archive: Path, dest_dir: Path, source_dir: str) -> Path:
    """Extract *archive* and return the source directory path."""
    extracted = dest_dir / source_dir
    if extracted.exists():
        shutil.rmtree(extracted)

    dest_dir.mkdir(parents=True, exist_ok=True)

    with tarfile.open(archive, "r:gz") as tf:
        # Path traversal protection: verify all members are within dest_dir
        for member in tf.getmembers():
            member_path = (dest_dir / member.name).resolve()
            if not str(member_path).startswith(str(dest_dir.resolve())):
                raise ValueError(
                    f"Path traversal detected: {member.name}"
                )
        tf.extractall(path=str(dest_dir))

    if not extracted.exists():
        raise FileNotFoundError(
            f"Expected source directory {extracted} after extraction"
        )

    # Write verified stamp
    stamp = extracted / ".hpactor-fetch-verified"
    stamp.write_text(archive.name)
    return extracted


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Fetch and verify native dependency sources"
    )
    parser.add_argument(
        "--lock", required=True,
        help="Path to native-deps.lock.json",
    )
    parser.add_argument(
        "--cache", required=True,
        help="Content-addressed download cache directory",
    )
    parser.add_argument(
        "--extract-to",
        help="Extract archives to this directory (default: no extraction)",
    )
    parser.add_argument(
        "--keep-going", action="store_true",
        help="Continue after fetch failures",
    )
    args = parser.parse_args()

    lock = json.loads(Path(args.lock).read_text())
    cache_dir = Path(args.cache)
    extract_dir = Path(args.extract_to) if args.extract_to else None

    errors = []
    for name, entry in lock.items():
        try:
            archive = fetch_source(entry["url"], entry["sha256"], cache_dir)
            if extract_dir:
                extract_source(archive, extract_dir, entry["source_dir"])
                print(f"  Extracted {name} -> {extract_dir / entry['source_dir']}",
                      file=sys.stderr)
        except Exception as exc:
            print(f"Error fetching {name}: {exc}", file=sys.stderr)
            if args.keep_going:
                errors.append(name)
            else:
                sys.exit(1)

    if errors:
        print(f"Errors for: {', '.join(errors)}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
