"""Tests for Phase 1D Task 3: Hermetic native dependency lock manifest."""

import hashlib
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent.parent.parent.parent


LOCK = _repo_root() / "bindings" / "python" / "packaging" / "native-deps.lock.json"


class NativeDependencyLockTest(unittest.TestCase):
    """RED phase — these tests must fail before Task 3 implementation."""

    def test_lock_file_exists_and_is_valid_json(self) -> None:
        self.assertTrue(LOCK.exists(), f"Lock file not found at {LOCK}")
        data = json.loads(LOCK.read_text())
        self.assertIsInstance(data, dict)
        self.assertEqual(data.get("version"), 1, "Lock file must have version=1")

    def test_every_source_is_https_and_sha256_locked(self) -> None:
        lock = json.loads(LOCK.read_text())
        self.assertIn("openssl", lock)
        self.assertIn("abseil", lock)
        self.assertIn("protobuf", lock)
        deps = ["openssl", "abseil", "protobuf"]
        for key in deps:
            entry = lock[key]
            self.assertTrue(
                entry["url"].startswith("https://"),
                f"{key}: URL must be HTTPS, got {entry['url']}",
            )
            self.assertRegex(
                entry["sha256"], r"^[0-9a-f]{64}$",
                f"{key}: sha256 must be 64 hex chars",
            )
            self.assertIn("version", entry)
            self.assertIn("license", entry)
            self.assertIn("source_dir", entry)

    def test_versions_are_exact_not_ranges(self) -> None:
        import re
        lock = json.loads(LOCK.read_text())
        deps = ["openssl", "abseil", "protobuf"]
        for key in deps:
            v = lock[key]["version"]
            self.assertIsInstance(v, str, f"{key} version must be a string")
            self.assertTrue(
                re.match(r"^\d+(\.\d+)*$", v),
                f"{key} version must be exact (no range operators), got: {v}",
            )

    def test_fetch_source_script_exists(self) -> None:
        fetch_script = (
            _repo_root()
            / "bindings"
            / "python"
            / "packaging"
            / "fetch_source.py"
        )
        self.assertTrue(
            fetch_script.exists(),
            f"fetch_source.py not found at {fetch_script}",
        )

    def test_build_native_deps_script_exists(self) -> None:
        build_script = (
            _repo_root()
            / "bindings"
            / "python"
            / "packaging"
            / "build_native_deps.py"
        )
        self.assertTrue(
            build_script.exists(),
            f"build_native_deps.py not found at {build_script}",
        )


if __name__ == "__main__":
    unittest.main()
