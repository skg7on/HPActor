"""Tests for Phase 1D Task 6: CI wheel matrix workflow."""

import re
import unittest
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent.parent.parent.parent


WORKFLOW = _repo_root() / ".github" / "workflows" / "python-wheels.yml"


class WheelCiMatrixTest(unittest.TestCase):
    def test_workflow_exists(self) -> None:
        self.assertTrue(
            WORKFLOW.exists(),
            f"python-wheels.yml not found at {WORKFLOW}",
        )

    def test_supported_targets_present(self) -> None:
        """Workflow contains the macOS ARM64 wheel build job."""
        text = WORKFLOW.read_text()
        self.assertIn("macosx_12_0_arm64", text,
                      "Workflow must reference macOS ARM64 wheel target")
        self.assertIn("cibuildwheel", text,
                      "Workflow must use cibuildwheel")

    def test_workflow_has_callable_trigger(self) -> None:
        """Wheel workflow must declare workflow_call for reuse."""
        text = WORKFLOW.read_text()
        self.assertIn("workflow_call", text,
                      "Workflow must declare workflow_call trigger")

    def test_wheel_workflow_not_named_publish(self) -> None:
        """The wheel workflow name must not contain 'publish'."""
        text = WORKFLOW.read_text()
        # Extract the workflow-level name: field (first name: after 'on:')
        m = re.search(r"^name:\s*(.+)", text, re.MULTILINE)
        if m:
            self.assertNotIn("publish", m.group(1).lower(),
                             f"Workflow name '{m.group(1)}' must not contain 'publish'")


if __name__ == "__main__":
    unittest.main()
