"""Verify installed hpactor wheel metadata.

Checks that distribution metadata is self-consistent and the version
reported by the installed package matches importlib.metadata.
"""

import unittest

try:
    from importlib.metadata import PackageNotFoundError, metadata, version
    HAS_IMPORTLIB_METADATA = True
except ImportError:
    HAS_IMPORTLIB_METADATA = False


class WheelMetadataTest(unittest.TestCase):
    """Verify the installed wheel's metadata."""

    def test_version_matches_distribution(self) -> None:
        """hpactor.__version__ must match the installed distribution."""
        if not HAS_IMPORTLIB_METADATA:
            raise unittest.SkipTest("importlib.metadata not available")
        try:
            dist_version = version("hpactor")
        except PackageNotFoundError:
            raise unittest.SkipTest(
                "hpactor not installed as a distribution "
                "(running from build tree)"
            )
        import hpactor
        self.assertEqual(hpactor.__version__, dist_version)

    def test_requires_python_declared(self) -> None:
        """Distribution metadata must declare Requires-Python."""
        if not HAS_IMPORTLIB_METADATA:
            raise unittest.SkipTest("importlib.metadata not available")
        try:
            meta = metadata("hpactor")
        except PackageNotFoundError:
            raise unittest.SkipTest("hpactor not installed as a distribution")
        requires_python = meta.get("Requires-Python")
        self.assertIsNotNone(requires_python,
                             "Requires-Python not set in distribution metadata")
        # Must be a CPython 3.11+ requirement
        self.assertIn("3.11", requires_python)

    def test_package_name_is_hpactor(self) -> None:
        """Distribution name must be hpactor."""
        if not HAS_IMPORTLIB_METADATA:
            raise unittest.SkipTest("importlib.metadata not available")
        try:
            meta = metadata("hpactor")
        except PackageNotFoundError:
            raise unittest.SkipTest("hpactor not installed as a distribution")
        self.assertEqual(meta.get("Name"), "hpactor")

    def test_py_typed_in_package(self) -> None:
        """hpactor/py.typed marker must be in the installed package."""
        import hpactor
        pkg_dir = hpactor.__path__[0]
        import os
        self.assertTrue(
            os.path.exists(os.path.join(pkg_dir, "py.typed")),
            "py.typed marker not found in installed hpactor package",
        )
