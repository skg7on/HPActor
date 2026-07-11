"""Tests for Phase 1D Task 1: Package metadata, versioning, and sdist contents."""

import importlib.util
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _repo_root() -> Path:
    """Return the repository root (two levels above this test file)."""
    return Path(__file__).resolve().parent.parent.parent.parent.parent


def build_sdist() -> Path:
    """Build a source distribution and return its Path.

    Uses ``python -m build --sdist`` in the repo root.  The resulting
    ``.tar.gz`` is expected to appear under ``dist/``.
    """
    root = _repo_root()
    subprocess.run(
        [sys.executable, "-m", "build", "--sdist"],
        cwd=str(root),
        check=True,
        capture_output=True,
    )
    dist_dir = root / "dist"
    archives = sorted(dist_dir.glob("hpactor-*.tar.gz"))
    if not archives:
        raise FileNotFoundError("No sdist archive found in dist/")
    return archives[-1]  # newest


def tar_names(archive: Path):
    """Return member names with the common top-level prefix stripped.

    An sdist archive typically contains a single top-level directory
    (e.g. ``hpactor-0.1.0/``).  This function strips that prefix so
    callers can assert on repo-relative paths.
    """
    with tarfile.open(archive, "r:gz") as tf:
        raw = {m.name for m in tf.getmembers()}

    # Find the common parent directory prefix
    prefixes = {n.split("/", 1)[0] for n in raw if "/" in n}
    if len(prefixes) == 1:
        prefix = prefixes.pop() + "/"
        return {n[len(prefix):] for n in raw if n.startswith(prefix)}
    return raw


class PackageMetadataTest(unittest.TestCase):
    """RED phase — these tests must fail before Task 1 implementation begins."""

    # ------------------------------------------------------------------
    # pyproject.toml existence and content
    # ------------------------------------------------------------------

    def test_pyproject_declares_supported_contract(self) -> None:
        """The project table declares name, python requirement, and
        protobuf dependency."""
        try:
            import tomllib
        except ImportError:
            import tomli as tomllib  # type: ignore[no-redef]

        pyproject = _repo_root() / "pyproject.toml"
        self.assertTrue(
            pyproject.exists(), f"pyproject.toml not found at {pyproject}"
        )

        data = tomllib.loads(pyproject.read_text())
        project = data["project"]
        self.assertEqual(project["name"], "hpactor")
        self.assertEqual(project["requires-python"], ">=3.11")
        self.assertIn("protobuf", project["dependencies"][0])

        scikit = data["tool"]["scikit-build"]
        self.assertEqual(scikit["wheel"]["py-api"], "cp311")

    # ------------------------------------------------------------------
    # sdist contents
    # ------------------------------------------------------------------

    def test_sdist_has_inputs_but_no_build_outputs(self) -> None:
        """The sdist includes critical source inputs but no build artefacts."""
        archive = build_sdist()
        names = tar_names(archive)

        # Required source inputs
        self.assertIn("pyproject.toml", names)
        self.assertIn("CMakeLists.txt", names)
        self.assertIn(
            "bindings/python/hpactor/__init__.py", names,
            "sdist must include the hpactor package"
        )
        self.assertIn(
            "protos/hpactor/python_binding_internal.proto", names,
            "sdist must include binding proto definitions"
        )

        # Forbidden build outputs
        build_basenames = [
            n for n in names if "/build/" in n
        ]
        self.assertFalse(
            build_basenames,
            f"sdist must not contain build artifacts: {build_basenames[:5]}"
        )
        for name in names:
            self.assertFalse(
                name.endswith((".o", ".a", ".so", ".dylib")),
                f"sdist must not contain compiled object: {name}",
            )

    # ------------------------------------------------------------------
    # Version module
    # ------------------------------------------------------------------

    def test_version_module_exists(self) -> None:
        """hpactor._version provides __version__ via
        importlib.metadata with a fallback."""
        version_path = (
            _repo_root() / "bindings" / "python" / "hpactor" / "_version.py"
        )
        self.assertTrue(
            version_path.exists(),
            f"_version.py not found at {version_path}",
        )

    def test_version_fallback_when_not_installed(self) -> None:
        """When the package is not installed, __version__ returns the
        unknown sentinel."""
        # Importing directly from source should give the fallback because
        # the package won't be installed via pip.
        spec = importlib.util.spec_from_file_location(
            "hpactor._version",
            _repo_root()
            / "bindings"
            / "python"
            / "hpactor"
            / "_version.py",
        )
        mod = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(mod)
        except Exception:
            # If the module doesn't exist yet the RED phase is working.
            raise
        self.assertEqual(mod.__version__, "0+unknown")


# ---------------------------------------------------------------------------
# Bootstrap for direct invocation
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main()
