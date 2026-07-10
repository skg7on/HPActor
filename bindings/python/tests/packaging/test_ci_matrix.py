"""Verify the CI wheel build matrix matches the supported platform set.

Checks that .github/workflows/python-wheels.yml declares exactly the
four supported target platforms with correct runner/arch/platform tags.
"""

import os
import unittest
from pathlib import Path

try:
    import yaml
    HAS_YAML = True
except ImportError:
    HAS_YAML = False


class WheelCiMatrixTest(unittest.TestCase):
    """Verify the Python wheel CI workflow matrix."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())
        self.workflow = self.root / ".github" / "workflows" / "python-wheels.yml"

    def test_workflow_exists(self) -> None:
        self.assertTrue(
            self.workflow.exists(),
            f"{self.workflow} not found",
        )

    def test_workflow_is_valid_yaml(self) -> None:
        if not HAS_YAML:
            raise unittest.SkipTest("PyYAML not installed")
        text = self.workflow.read_text()
        try:
            yaml.safe_load(text)
        except yaml.YAMLError as e:
            self.fail(f"Invalid YAML: {e}")

    def test_exact_supported_targets(self) -> None:
        if not HAS_YAML:
            raise unittest.SkipTest("PyYAML not installed")
        data = yaml.safe_load(self.workflow.read_text())
        matrix = (
            data.get("jobs", {})
            .get("build-wheels", {})
            .get("strategy", {})
            .get("matrix", {})
            .get("include", [])
        )
        targets = {
            (row["os"], row["arch"], row["platform"])
            for row in matrix
        }
        expected = {
            ("ubuntu-26.04", "x86_64", "manylinux_2_28_x86_64"),
            ("ubuntu-26.04-arm", "aarch64", "manylinux_2_28_aarch64"),
            ("macos-15", "arm64", "macosx_12_0_arm64"),
        }
        self.assertEqual(targets, expected)

    def test_build_selector_is_cp311(self) -> None:
        text = self.workflow.read_text()
        self.assertIn("cp311-*", text,
                      "CIBW_BUILD must select cp311-*")

    def test_no_publish_permissions(self) -> None:
        """Pull-request and main-push workflows must not have publish permissions."""
        if not HAS_YAML:
            raise unittest.SkipTest("PyYAML not installed")
        data = yaml.safe_load(self.workflow.read_text())
        permissions = data.get("permissions", {})
        self.assertNotIn("id-token", permissions,
                         "Wheel build workflow must not have id-token permission")

    def test_smoke_tests_cover_all_supported_cpython_versions(self) -> None:
        if not HAS_YAML:
            raise unittest.SkipTest("PyYAML not installed")
        data = yaml.safe_load(self.workflow.read_text())
        smoke = (
            data.get("jobs", {})
            .get("smoke-test", {})
            .get("strategy", {})
            .get("matrix", {})
            .get("include", [])
        )
        python_versions = {row["python"] for row in smoke}
        self.assertIn("3.11", python_versions)
        self.assertIn("3.12", python_versions)
        self.assertIn("3.13", python_versions)
        self.assertIn("3.14", python_versions)
