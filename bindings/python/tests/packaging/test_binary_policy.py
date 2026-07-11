"""Tests for Phase 1D Task 4: Wheel content and binary policy verification."""

import json
import unittest
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent.parent.parent.parent


POLICY = _repo_root() / "bindings" / "python" / "packaging" / "dependency-policy.json"
VERIFY = _repo_root() / "bindings" / "python" / "packaging" / "verify_wheel.py"


class WheelContentsTest(unittest.TestCase):
    """Verify wheel audit tooling exists and is properly configured."""

    def test_policy_file_exists(self) -> None:
        self.assertTrue(POLICY.exists(), f"Policy file not found at {POLICY}")

    def test_policy_is_valid_json(self) -> None:
        data = json.loads(POLICY.read_text())
        self.assertIn("required_private_libraries", data)
        self.assertIn("forbidden_unresolved_prefixes", data)
        self.assertIn("targets", data)
        targets = data["targets"]
        self.assertIn("macosx_12_0_arm64", targets)

    def test_verify_script_exists(self) -> None:
        self.assertTrue(VERIFY.exists(), f"verify_wheel.py not found at {VERIFY}")

    def test_wheel_tag_is_abi3(self) -> None:
        data = json.loads(POLICY.read_text())
        tags = data["wheel_tags"]
        self.assertEqual(tags["python_tag"], "cp311")
        self.assertEqual(tags["abi_tag"], "abi3")

    def test_forbidden_roots_catch_build_leakage(self) -> None:
        data = json.loads(POLICY.read_text())
        forbidden = data["forbidden_absolute_roots"]
        # Paths that would indicate a build leak
        self.assertIn("/home/", forbidden)
        self.assertIn("/opt/homebrew", forbidden)


class BinaryPolicyTest(unittest.TestCase):
    """Verify wheel binary policy rejects known-bad patterns."""

    def test_fake_absolute_rpath_is_rejected(self) -> None:
        """A hardcoded absolute RPATH would violate the policy."""
        data = json.loads(POLICY.read_text())
        forbidden = data["forbidden_absolute_roots"]
        fake_rpath = "/home/runner/work/HPActor/build/deps/lib"
        self.assertTrue(
            any(fake_rpath.startswith(prefix) for prefix in forbidden),
            "Fake build rpath must be rejected by policy",
        )

    def test_forbidden_unresolved_prefixes_cover_all_deps(self) -> None:
        data = json.loads(POLICY.read_text())
        prefixes = data["forbidden_unresolved_prefixes"]
        self.assertIn("hpactor", prefixes)
        self.assertIn("protobuf", prefixes)
        self.assertIn("absl", prefixes)
        self.assertIn("ssl", prefixes)
        self.assertIn("crypto", prefixes)


if __name__ == "__main__":
    unittest.main()
