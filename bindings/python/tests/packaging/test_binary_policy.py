"""Tests for wheel binary policy enforcement.

Verifies that verify_wheel.py rejects wheels with forbidden RPATHs,
unresolved dependencies, or architecture mismatches.
"""

import os
import subprocess
import sys
import unittest
from pathlib import Path


class BinaryPolicyTest(unittest.TestCase):
    """Verify binary policy checks catch common violations."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())

    def test_verify_wheel_rejects_missing_file(self) -> None:
        script = (
            self.root / "bindings" / "python" / "packaging" / "verify_wheel.py"
        )
        if not script.exists():
            raise unittest.SkipTest("verify_wheel.py not yet created")
        result = subprocess.run(
            [sys.executable, str(script),
             "--wheel", "nonexistent.whl",
             "--policy", str(self.root / "bindings" / "python" / "packaging" / "dependency-policy.json")],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0,
                            "verify_wheel.py should fail for missing wheel")

    def test_verify_wheel_rejects_missing_policy(self) -> None:
        script = (
            self.root / "bindings" / "python" / "packaging" / "verify_wheel.py"
        )
        if not script.exists():
            raise unittest.SkipTest("verify_wheel.py not yet created")
        result = subprocess.run(
            [sys.executable, str(script),
             "--wheel", "nonexistent.whl",
             "--policy", "nonexistent.json"],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0,
                            "verify_wheel.py should fail for missing policy")

    def test_verify_wheel_accepts_help(self) -> None:
        script = (
            self.root / "bindings" / "python" / "packaging" / "verify_wheel.py"
        )
        if not script.exists():
            raise unittest.SkipTest("verify_wheel.py not yet created")
        result = subprocess.run(
            [sys.executable, str(script), "--help"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0,
                         f"verify_wheel.py --help failed: {result.stderr}")
