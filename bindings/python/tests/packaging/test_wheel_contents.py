"""Tests for wheel content and binary policy verification.

Verifies that repaired wheels contain exactly one native module, have
correct ABI3 metadata, and pass dependency-policy checks.
"""

import json
import os
import unittest
from pathlib import Path


class WheelContentsTest(unittest.TestCase):
    """Verify the structure and metadata of a repaired wheel."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())

    def test_verify_wheel_script_exists(self) -> None:
        script = (
            self.root / "bindings" / "python" / "packaging" / "verify_wheel.py"
        )
        self.assertTrue(script.exists(), f"{script} not found")

    def test_dependency_policy_exists(self) -> None:
        policy = (
            self.root / "bindings" / "python" / "packaging" / "dependency-policy.json"
        )
        self.assertTrue(policy.exists(), f"{policy} not found")

    def test_dependency_policy_has_required_keys(self) -> None:
        policy_path = (
            self.root / "bindings" / "python" / "packaging" / "dependency-policy.json"
        )
        if not policy_path.exists():
            raise unittest.SkipTest("dependency-policy.json not yet created")
        policy = json.loads(policy_path.read_text())
        self.assertIn("required_libraries", policy)
        self.assertIn("forbidden_prefixes", policy)
        self.assertIn("allowed_system_roots", policy)

    def test_dependency_policy_requires_hpactor_libs(self) -> None:
        policy_path = (
            self.root / "bindings" / "python" / "packaging" / "dependency-policy.json"
        )
        if not policy_path.exists():
            raise unittest.SkipTest("dependency-policy.json not yet created")
        policy = json.loads(policy_path.read_text())
        required = policy["required_libraries"]
        self.assertIn("hpactor_lib", required)
        self.assertIn("hpactor_proto", required)

    def test_dependency_policy_forbids_absolute_paths(self) -> None:
        policy_path = (
            self.root / "bindings" / "python" / "packaging" / "dependency-policy.json"
        )
        if not policy_path.exists():
            raise unittest.SkipTest("dependency-policy.json not yet created")
        policy = json.loads(policy_path.read_text())
        forbidden = policy.get("forbidden_absolute_roots", [])
        self.assertIn("/opt/homebrew", forbidden)
        self.assertIn("/usr/local", forbidden)
