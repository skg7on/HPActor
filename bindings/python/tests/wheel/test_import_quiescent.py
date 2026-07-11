"""Verify importing hpactor does not create threads or file descriptors."""

import os
import threading
import unittest


def _fd_count() -> int:
    """Return the number of open file descriptors (Linux only)."""
    try:
        return len(os.listdir("/proc/self/fd"))
    except (FileNotFoundError, PermissionError):
        return -1


class ImportQuiescentTest(unittest.TestCase):
    def test_import_creates_no_threads(self) -> None:
        before = {t.ident for t in threading.enumerate()}
        import hpactor  # noqa: F401
        import hpactor._hpactor  # noqa: F401
        after = {t.ident for t in threading.enumerate()}
        self.assertEqual(after, before,
                         "Import must not create threads")

    def test_import_creates_no_file_descriptors(self) -> None:
        before = _fd_count()
        if before < 0:
            self.skipTest("fd count not available")
        # Re-import (already imported, no-op)
        after = _fd_count()
        self.assertEqual(after, before,
                         "Import must not open file descriptors")


if __name__ == "__main__":
    unittest.main()
