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

    def test_exact_supported_targets(self) -> None:
        if yaml is None:
            self.skipTest("PyYAML not installed")
        wf = _load_yaml(WORKFLOW)
        matrix = (
            wf.get("jobs", {})
            .get("build-wheels", {})
            .get("strategy", {})
            .get("matrix", {})
            .get("include", [])
        )
        targets = {
            (row["os"], row["arch"], row["platform"])
            for row in matrix
        }
        self.assertEqual(targets, {
            ("ubuntu-24.04", "x86_64", "manylinux_2_28_x86_64"),
            ("ubuntu-24.04-arm", "aarch64", "manylinux_2_28_aarch64"),
            ("macos-15", "x86_64", "macosx_12_0_x86_64"),
            ("macos-14", "arm64", "macosx_12_0_arm64"),
        })

    def test_no_publish_permissions(self) -> None:
        text = WORKFLOW.read_text()
        # Wheel workflow must not have publish permissions
        self.assertNotIn("id-token: write", text,
                         "Wheel workflow must not have publish permissions")
        self.assertNotIn("publish", text.lower().split("name:")[0],
                         "Wheel workflow must not be named publish")


if __name__ == "__main__":
    unittest.main()
