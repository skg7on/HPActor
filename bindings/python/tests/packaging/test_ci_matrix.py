"""Tests for Phase 1D Task 6: CI wheel matrix workflow."""

import unittest
from pathlib import Path

try:
    import yaml
except ImportError:
    yaml = None  # type: ignore[assignment]


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent.parent.parent.parent


WORKFLOW = _repo_root() / ".github" / "workflows" / "python-wheels.yml"


def _load_yaml(path: Path) -> dict:
    if yaml is not None:
        with open(path) as fh:
            return yaml.safe_load(fh)
    # Fallback: use a minimal heuristic parser
    text = path.read_text()
    result = {}
    for line in text.splitlines():
        if line.startswith("name:"):
            result["name"] = line.split(":", 1)[1].strip()
    return result


class WheelCiMatrixTest(unittest.TestCase):
    def test_workflow_exists(self) -> None:
        self.assertTrue(
            WORKFLOW.exists(),
            f"python-wheels.yml not found at {WORKFLOW}",
        )

    def test_supported_targets_present(self) -> None:
        """Workflow contains at least one wheel build job."""
        text = WORKFLOW.read_text()
        self.assertIn("macosx_12_0_arm64", text,
                      "Workflow must reference macOS ARM64 wheel target")
        self.assertIn("cibuildwheel", text,
                      "Workflow must use cibuildwheel")

    def test_no_publish_permissions(self) -> None:
        text = WORKFLOW.read_text()
        # Wheel workflow must not have publish permissions
        self.assertNotIn("id-token: write", text,
                         "Wheel workflow must not have publish permissions")
        self.assertNotIn("publish", text.lower().split("name:")[0],
                         "Wheel workflow must not be named publish")


if __name__ == "__main__":
    unittest.main()
