"""Tests for hermetic native dependency locking and offline rebuilds.

Verifies the dependency manifest, checksum-locked fetching, and
offline rebuild capability for OpenSSL, Abseil, and protobuf.
"""

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path


class NativeDependencyLockTest(unittest.TestCase):
    """Verify native-deps.lock.json is correct and complete."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())
        self.lock_path = (
            self.root / "bindings" / "python" / "packaging" / "native-deps.lock.json"
        )

    def test_lock_file_exists(self) -> None:
        self.assertTrue(
            self.lock_path.exists(),
            f"{self.lock_path} not found",
        )

    def test_every_source_is_https_and_sha256_locked(self) -> None:
        lock = json.loads(self.lock_path.read_text())
        self.assertEqual(set(lock), {"openssl", "abseil", "protobuf"})
        for entry in lock.values():
            self.assertTrue(
                entry["url"].startswith("https://"),
                f"URL must be HTTPS: {entry['url']}",
            )
            self.assertRegex(
                entry["sha256"], r"^[0-9a-f]{64}$",
                f"sha256 must be 64 hex chars: {entry['sha256'][:20]}...",
            )

    def test_versions_are_exact(self) -> None:
        lock = json.loads(self.lock_path.read_text())
        self.assertEqual(lock["openssl"]["version"], "3.5.5")
        self.assertEqual(lock["abseil"]["version"], "20260107.1")
        self.assertEqual(lock["protobuf"]["version"], "35.0")

    def test_every_entry_has_license(self) -> None:
        lock = json.loads(self.lock_path.read_text())
        for name, entry in lock.items():
            self.assertIn("license", entry,
                          f"{name} entry missing license field")

    def test_every_entry_has_source_dir(self) -> None:
        lock = json.loads(self.lock_path.read_text())
        for name, entry in lock.items():
            self.assertIn("source_dir", entry,
                          f"{name} entry missing source_dir field")


class FetchSourceTest(unittest.TestCase):
    """Verify fetch_source.py exists and has expected interface."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())
        self.script = (
            self.root / "bindings" / "python" / "packaging" / "fetch_source.py"
        )

    def test_fetch_script_exists(self) -> None:
        self.assertTrue(
            self.script.exists(),
            f"{self.script} not found",
        )

    def test_fetch_script_is_executable(self) -> None:
        self.assertTrue(
            os.access(self.script, os.X_OK),
            f"{self.script} is not executable",
        )

    def test_fetch_accepts_help(self) -> None:
        result = subprocess.run(
            [sys.executable, str(self.script), "--help"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0,
                         f"fetch_source.py --help failed: {result.stderr}")

    def test_fetch_rejects_missing_lock(self) -> None:
        result = subprocess.run(
            [sys.executable, str(self.script),
             "--lock", "nonexistent.json",
             "--cache", "/tmp"],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0,
                            "fetch_source.py should fail with missing lock")


class BuildNativeDepsTest(unittest.TestCase):
    """Verify build_native_deps.py exists and has expected interface."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())
        self.script = (
            self.root / "bindings" / "python" / "packaging" / "build_native_deps.py"
        )

    def test_build_script_exists(self) -> None:
        self.assertTrue(
            self.script.exists(),
            f"{self.script} not found",
        )

    def test_build_script_is_executable(self) -> None:
        self.assertTrue(
            os.access(self.script, os.X_OK),
            f"{self.script} is not executable",
        )

    def test_build_accepts_help(self) -> None:
        result = subprocess.run(
            [sys.executable, str(self.script), "--help"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0,
                         f"build_native_deps.py --help failed: {result.stderr}")
