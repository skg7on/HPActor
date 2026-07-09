"""Tests for hpactor Python distribution metadata and sdist contents.

These tests verify that pyproject.toml declares the correct package contract
and that the source distribution contains inputs but no compiled outputs.
"""

import os
import sys
import tarfile
import unittest
from pathlib import Path


class PackageMetadataTest(unittest.TestCase):
    """Verify pyproject.toml declares the supported distribution contract."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())

    def test_pyproject_exists_and_is_valid_toml(self) -> None:
        pyproject = self.root / "pyproject.toml"
        self.assertTrue(pyproject.exists(),
                        f"pyproject.toml not found at {pyproject}")
        import tomllib
        data = tomllib.loads(pyproject.read_text())
        self.assertIn("project", data)
        self.assertIn("build-system", data)

    def test_project_name_is_hpactor(self) -> None:
        import tomllib
        data = tomllib.loads((self.root / "pyproject.toml").read_text())
        self.assertEqual(data["project"]["name"], "hpactor")

    def test_requires_python_at_least_3_11(self) -> None:
        import tomllib
        data = tomllib.loads((self.root / "pyproject.toml").read_text())
        self.assertEqual(data["project"]["requires-python"], ">=3.11")

    def test_dependencies_declare_protobuf(self) -> None:
        import tomllib
        data = tomllib.loads((self.root / "pyproject.toml").read_text())
        self.assertEqual(data["project"]["dependencies"],
                         ["protobuf>=7.35.0,<8"])

    def test_wheel_py_api_is_cp311(self) -> None:
        import tomllib
        data = tomllib.loads((self.root / "pyproject.toml").read_text())
        self.assertEqual(data["tool"]["scikit-build"]["wheel"]["py-api"],
                         "cp311")

    def test_build_backend_is_scikit_build_core(self) -> None:
        import tomllib
        data = tomllib.loads((self.root / "pyproject.toml").read_text())
        self.assertEqual(data["build-system"]["build-backend"],
                         "scikit_build_core.build")

    def test_sdist_can_be_built(self) -> None:
        """sdist builds without error."""
        archive_path = _build_sdist(self.root)
        self.assertTrue(archive_path.exists(),
                        f"sdist archive not found at {archive_path}")
        self.assertTrue(archive_path.stat().st_size > 0,
                        "sdist archive is empty")

    def test_sdist_contains_required_sources(self) -> None:
        """sdist must include sources needed for an end-user build."""
        archive_path = _build_sdist(self.root)
        names = _tar_names(archive_path)
        required = [
            "pyproject.toml",
            "CMakeLists.txt",
            "bindings/python/hpactor/__init__.py",
        ]
        for f in required:
            self.assertIn(f, names, f"sdist missing required file: {f}")

    def test_sdist_has_no_compiled_objects(self) -> None:
        """sdist must not contain compiled or linked artifacts."""
        archive_path = _build_sdist(self.root)
        names = _tar_names(archive_path)
        compiled_exts = (".o", ".a", ".so", ".dylib", ".dll")
        compiled = [n for n in names if n.endswith(compiled_exts)]
        self.assertEqual(len(compiled), 0,
                         f"sdist contains compiled artifacts: {compiled}")


class VersionTest(unittest.TestCase):
    """Verify hpactor.__version__ is wired to the installed distribution."""

    def test_version_module_exists(self) -> None:
        version_file = Path("bindings/python/hpactor/_version.py")
        self.assertTrue(version_file.exists(),
                        f"{version_file} not found")

    def test_version_uses_importlib_metadata(self) -> None:
        text = Path("bindings/python/hpactor/_version.py").read_text()
        self.assertIn("importlib.metadata", text)
        self.assertIn('version("hpactor")', text)
        self.assertIn("PackageNotFoundError", text)

    def test_version_fallback_is_0_unknown(self) -> None:
        text = Path("bindings/python/hpactor/_version.py").read_text()
        self.assertIn("0+unknown", text)

    def test_init_exports_version(self) -> None:
        text = Path("bindings/python/hpactor/__init__.py").read_text()
        self.assertIn("__version__", text)


class ReadmeTest(unittest.TestCase):
    """Verify the package README exists and contains required sections."""

    def test_readme_exists(self) -> None:
        readme = Path("bindings/python/README.md")
        self.assertTrue(readme.exists(), f"{readme} not found")

    def test_readme_mentions_supported_platforms(self) -> None:
        text = Path("bindings/python/README.md").read_text()
        self.assertIn("Linux", text)
        self.assertIn("macOS", text)

    def test_readme_mentions_cpython_requirement(self) -> None:
        text = Path("bindings/python/README.md").read_text()
        self.assertIn("3.11", text)

    def test_readme_mentions_installation(self) -> None:
        text = Path("bindings/python/README.md").read_text()
        self.assertIn("pip install", text)


# --- helpers ---------------------------------------------------------------


def _build_sdist(root: Path) -> Path:
    """Build an sdist with python -m build and return the archive path."""
    import subprocess
    dist_dir = root / "dist"
    dist_dir.mkdir(exist_ok=True)
    for p in dist_dir.glob("*.tar.gz"):
        p.unlink()
    subprocess.run(
        [sys.executable, "-m", "build", "--sdist", "--outdir", str(dist_dir)],
        cwd=str(root),
        check=True,
        capture_output=True,
    )
    archives = list(dist_dir.glob("*.tar.gz"))
    if not archives:
        raise FileNotFoundError("No sdist archive produced")
    return archives[0]


def _tar_names(archive: Path) -> list[str]:
    """Return the member names in a tar.gz archive, with the common
    top-level directory prefix stripped."""
    with tarfile.open(archive, "r:gz") as tf:
        raw = tf.getnames()
    prefix = os.path.commonpath(raw)
    stripped = []
    for name in raw:
        if name == prefix:
            continue
        rel = name[len(prefix) + 1:]
        stripped.append(rel)
    return stripped
