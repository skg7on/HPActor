"""Verify installed wheel metadata."""

import unittest

try:
    from importlib.metadata import PackageNotFoundError, distribution
except ImportError:
    PackageNotFoundError = None  # type: ignore[assignment]
    distribution = None  # type: ignore[assignment]


class WheelMetadataTest(unittest.TestCase):
    def test_version_is_not_unknown(self) -> None:
        import hpactor
        self.assertIsNotNone(hpactor.__version__)
        self.assertNotEqual(hpactor.__version__, "0+unknown",
                            "Installed wheel must report real version")

    def test_package_metadata_present(self) -> None:
        if distribution is None:
            self.skipTest("importlib.metadata not available")
        try:
            dist = distribution("hpactor")
        except (PackageNotFoundError, ModuleNotFoundError):
            self.skipTest("hpactor package not installed")
        self.assertEqual(dist.metadata["Name"], "hpactor")
        self.assertIn("protobuf", dist.metadata["Requires-Dist"])


if __name__ == "__main__":
    unittest.main()
