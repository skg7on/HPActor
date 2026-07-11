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

    def test_every_source_is_https_and_sha256_locked(self) -> None:
        lock = json.loads(LOCK.read_text())
        self.assertGreaterEqual(len(lock), 3, "Expected at least 3 deps")
        self.assertIn("openssl", lock)
        self.assertIn("abseil", lock)
        self.assertIn("protobuf", lock)
        for key, entry in lock.items():
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

    def test_versions_are_exact(self) -> None:
        lock = json.loads(LOCK.read_text())
        self.assertEqual(lock["openssl"]["version"], "3.5.5")
        self.assertEqual(lock["abseil"]["version"], "20260107.1")
        self.assertEqual(lock["protobuf"]["version"], "35.0")

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
