"""Verify the publish workflow enforces safety invariants.

Checks that the publishing workflow requires tags/acceptance, uses OIDC
trusted publishing, and never stores long-lived API tokens.
"""

import json
import os
import unittest
from pathlib import Path

try:
    import yaml
    HAS_YAML = True
except ImportError:
    HAS_YAML = False


class PublishWorkflowTest(unittest.TestCase):
    """Verify publish workflow safety invariants."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())
        self.workflow = self.root / ".github" / "workflows" / "python-publish.yml"

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

    def test_publish_requires_acceptance(self) -> None:
        if not HAS_YAML:
            raise unittest.SkipTest("PyYAML not installed")
        data = yaml.safe_load(self.workflow.read_text())
        pypi_job = data.get("jobs", {}).get("publish-pypi", {})
        needs = pypi_job.get("needs", [])
        self.assertIn("build", needs,
                      "publish-pypi must depend on build job")

    def test_publish_uses_oidc(self) -> None:
        if not HAS_YAML:
            raise unittest.SkipTest("PyYAML not installed")
        data = yaml.safe_load(self.workflow.read_text())

        pypi_job = data.get("jobs", {}).get("publish-pypi", {})
        permissions = pypi_job.get("permissions", {})
        self.assertEqual(
            permissions.get("id-token"), "write",
            "publish-pypi must have id-token: write",
        )

        testpypi_job = data.get("jobs", {}).get("publish-testpypi", {})
        permissions = testpypi_job.get("permissions", {})
        self.assertEqual(
            permissions.get("id-token"), "write",
            "publish-testpypi must have id-token: write",
        )

    def test_no_hardcoded_secrets(self) -> None:
        """Workflow must not contain hardcoded passwords or tokens."""
        text = self.workflow.read_text().lower()
        self.assertNotIn("password", text,
                         "Workflow must not contain 'password'")
        self.assertNotIn("token =", text,
                         "Workflow must not contain token assignments")

    def test_pypi_requires_protected_environment(self) -> None:
        if not HAS_YAML:
            raise unittest.SkipTest("PyYAML not installed")
        data = yaml.safe_load(self.workflow.read_text())
        pypi_job = data.get("jobs", {}).get("publish-pypi", {})
        env = pypi_job.get("environment", {})
        self.assertEqual(
            env.get("name"), "pypi",
            "publish-pypi must use the 'pypi' protected environment",
        )

    def test_tag_triggers_publish(self) -> None:
        """Workflow must trigger on v* version tags.
        Note: PyYAML parses 'on:' as boolean True, so we check raw text.
        """
        text = self.workflow.read_text()
        self.assertIn("tags:", text,
                      "Workflow must have tags: trigger")
        self.assertIn('"v[0-9]', text,
                      "Workflow must trigger on v* version tags")

    def test_releasing_guide_exists(self) -> None:
        guide = (
            self.root / "bindings" / "python" / "packaging" / "RELEASING.md"
        )
        self.assertTrue(guide.exists(), f"{guide} not found")

    def test_releasing_guide_documents_yank(self) -> None:
        guide = (
            self.root / "bindings" / "python" / "packaging" / "RELEASING.md"
        )
        text = guide.read_text()
        self.assertIn("yank", text.lower(),
                      "RELEASING.md must document yank procedure")
