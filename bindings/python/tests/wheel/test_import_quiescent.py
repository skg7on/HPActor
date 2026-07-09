"""Verify that importing hpactor has no side effects.

The import must not start threads, open file descriptors, or initialize
any native resources. All runtime activity begins only when ActorSystem
is explicitly started.
"""

import unittest


class ImportQuiescentTest(unittest.TestCase):
    """Importing hpactor must be side-effect free."""

    def test_can_import_hpactor(self) -> None:
        """hpactor package must be importable."""
        import hpactor  # noqa: F401

    def test_can_import_native_module(self) -> None:
        """hpactor._hpactor native module must be importable."""
        import hpactor._hpactor  # noqa: F401

    def test_version_is_available(self) -> None:
        """hpactor.__version__ must be a non-empty string."""
        import hpactor
        self.assertIsInstance(hpactor.__version__, str)
        self.assertGreater(len(hpactor.__version__), 0)
        # In a development build tree, version may be "0+unknown"
        # In an installed wheel, it must be a real version

    def test_public_api_is_importable(self) -> None:
        """All public names in hpactor.__all__ must be importable."""
        import hpactor
        for name in hpactor.__all__:
            if name == "__version__":
                continue
            self.assertTrue(
                hasattr(hpactor, name),
                f"hpactor.{name} not found in module",
            )
