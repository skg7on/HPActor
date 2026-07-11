"""Tests for Phase 1D Task 2: CMake wheel install layout."""

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent.parent.parent.parent


def _cmake_configure_and_build() -> Path:
    """Configure and build the python-wheel install component.

    Returns the staging prefix (build/python-layout/stage).
    """
    root = _repo_root()
    build_dir = root / "build" / "python-layout"
    build_dir.mkdir(parents=True, exist_ok=True)

    # Configure
    subprocess.run(
        [
            "cmake", "-S", str(root), "-B", str(build_dir), "-GNinja",
            "-DENABLE_PYTHON_BINDINGS=ON",
            "-DHPACTOR_PYTHON_WHEEL_BUILD=ON",
            "-DENABLE_TESTS=OFF",
            "-DENABLE_EXAMPLES=OFF",
            "-DENABLE_APPS=OFF",
        ],
        check=True,
        capture_output=True,
    )

    # Build only the extension module
    subprocess.run(
        ["ninja", "-C", str(build_dir), "_hpactor"],
        check=True,
        capture_output=True,
    )

    # Install to staging
    stage = build_dir / "stage"
    subprocess.run(
        [
            "cmake", "--install", str(build_dir),
            "--component", "python-wheel",
            "--prefix", str(stage),
        ],
        check=True,
        capture_output=True,
    )

    return stage


def _installed_extension(stage: Path) -> Path:
    """Return the single _hpactor*.so (or .dylib) under stage/hpactor/."""
    candidates = list(stage.glob("hpactor/_hpactor*.so"))
    if not candidates:
        candidates = list(stage.glob("hpactor/_hpactor*.dylib"))
    if not candidates:
        raise FileNotFoundError(
            f"No _hpactor extension found under {stage}/hpactor/"
        )
    return candidates[0]


class CMakeWheelLayoutTest(unittest.TestCase):
    """RED phase — these tests must fail before Task 2 implementation."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.stage = _cmake_configure_and_build()

    def test_staged_install_contains_only_runtime_files(self) -> None:
        """The python-wheel component contains one extension, private
        libraries, and no build/debug artefacts."""
        root = self.stage

        # One native extension module
        ext_count = len(list(root.glob("hpactor/_hpactor*.so"))) + len(
            list(root.glob("hpactor/_hpactor*.dylib"))
        )
        self.assertEqual(ext_count, 1, "Expected exactly one _hpactor extension")

        # Private libraries directory
        libs_dir = root / "hpactor" / ".libs"
        self.assertTrue(
            libs_dir.is_dir(),
            f"Expected hpactor/.libs directory at {libs_dir}",
        )

        # No static archives
        static_libs = list(root.rglob("*.a"))
        self.assertFalse(
            static_libs,
            f"Static archives found in wheel install: {static_libs}",
        )

        # No CMake cache files
        cmake_cache = list(root.rglob("CMakeCache.txt"))
        self.assertFalse(
            cmake_cache,
            "CMake cache files must not be installed",
        )

    def test_extension_has_no_build_output_artifacts(self) -> None:
        """No .o, .pyc, or other build artifacts in the install."""
        for suffix in (".o", ".pyc"):
            artifacts = list(self.stage.rglob(f"*{suffix}"))
            self.assertFalse(
                artifacts,
                f"Build artifact {suffix} found in install: {artifacts}",
            )

    def test_no_absolute_rpath_in_extensions(self) -> None:
        """Installed shared objects must not contain absolute RPATHs
        pointing to the checkout, build directory, or system prefixes."""
        forbidden_prefixes = [
            str(_repo_root()),
            str(_repo_root() / "build"),
            "/opt/homebrew",
            "/usr/local",
        ]
        for candidate in self.stage.rglob("*.so"):
            result = subprocess.run(
                ["readelf", "-d", str(candidate)],
                capture_output=True, text=True,
            )
            for line in result.stdout.splitlines():
                if "RPATH" in line or "RUNPATH" in line:
                    for prefix in forbidden_prefixes:
                        self.assertNotIn(
                            prefix, line,
                            f"Absolute RPATH in {candidate.name}: {line.strip()}"
                        )


# ---------------------------------------------------------------------------
if __name__ == "__main__":
    unittest.main()
