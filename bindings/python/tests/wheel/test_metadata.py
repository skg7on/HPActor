"""Verify installed wheel metadata."""

import unittest

try:
    from importlib.metadata import distribution, version
except ImportError:
    distribution = None  # type: ignore[assignment]
    version = None  # type: ignore[assignment]


class WheelMetadataTest(unittest.TestCase):
    def test_version_is_not_unknown(self) -> None:
        import hpactor
        self.assertIsNotNone(hpactor.__version__)
        self.assertNotEqual(hpactor.__version__, "0+unknown",
                            "Installed wheel must report real version")

    def test_package_metadata_present(self) -> None:
        if distribution is None:
            self.skipTest("importlib.metadata not available")
        dist = distribution("hpactor")
        self.assertEqual(dist.metadata["Name"], "hpactor")
        self.assertIn("protobuf", dist.metadata["Requires-Dist"])


if __name__ == "__main__":
    unittest.main()
