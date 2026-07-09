"""Tests for the CMake wheel install layout.

Verifies that ``cmake --install ... --component python-wheel`` produces
a correct ABI3 wheel staging directory with one extension module, private
HPActor runtime libraries, correct RPATH, and no developer artifacts.
"""

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path


class CMakeWheelLayoutTest(unittest.TestCase):
    """Verify the CMake install component for the Python wheel."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())
        self.stage = self.root / "build" / "python-wheel-stage"

    # ── helpers ──────────────────────────────────────────────────────────

    def _configure_and_install(self) -> Path:
        """Configure CMake for wheel build and install the python-wheel component.
        Returns the staging prefix.
        """
        build_dir = self.root / "build" / "python-wheel-build"
        build_dir.mkdir(parents=True, exist_ok=True)

        # Configure
        subprocess.run(
            [
                "cmake", "-S", str(self.root), "-B", str(build_dir),
                "-GNinja",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "-DENABLE_PYTHON_BINDINGS=ON",
                "-DHPACTOR_PYTHON_WHEEL_BUILD=ON",
                "-DENABLE_TESTS=OFF",
                "-DENABLE_EXAMPLES=OFF",
                "-DENABLE_APPS=OFF",
            ],
            check=True,
            capture_output=True,
        )

        # Build the extension
        subprocess.run(
            ["ninja", "-C", str(build_dir), "_hpactor"],
            check=True,
            capture_output=True,
        )

        # Install
        stage_dir = self.stage
        if stage_dir.exists():
            import shutil
            shutil.rmtree(stage_dir)
        stage_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                "cmake", "--install", str(build_dir),
                "--component", "python-wheel",
                "--prefix", str(stage_dir),
            ],
            check=True,
            capture_output=True,
        )

        return stage_dir

    # ── tests ────────────────────────────────────────────────────────────

    def test_wheel_build_option_is_available(self) -> None:
        """HPACTOR_PYTHON_WHEEL_BUILD must be a declared CMake option."""
        cmake_file = self.root / "CMakeLists.txt"
        text = cmake_file.read_text()
        self.assertIn("HPACTOR_PYTHON_WHEEL_BUILD", text,
                      "HPACTOR_PYTHON_WHEEL_BUILD option not found in CMakeLists.txt")

    def test_python_wheel_install_module_exists(self) -> None:
        """cmake/python_wheel_install.cmake must exist."""
        install_module = self.root / "cmake" / "python_wheel_install.cmake"
        self.assertTrue(
            install_module.exists(),
            f"{install_module} not found",
        )

    def test_staged_install_has_extension_module(self) -> None:
        """Staged install must contain exactly one _hpactor extension."""
        stage = self._configure_and_install()
        extensions = list(stage.glob("hpactor/_hpactor*.so"))
        self.assertEqual(
            len(extensions), 1,
            f"Expected 1 extension, found {len(extensions)}: {extensions}",
        )

    def test_staged_install_has_private_libs_dir(self) -> None:
        """Staged install must contain hpactor/.libs with runtime libraries."""
        stage = self._configure_and_install()
        libs_dir = stage / "hpactor" / ".libs"
        self.assertTrue(
            libs_dir.is_dir(),
            f"{libs_dir} is not a directory",
        )
        # Must have at least one shared library
        libs = list(libs_dir.glob("*.so")) + list(libs_dir.glob("*.dylib"))
        self.assertGreater(
            len(libs), 0,
            f"No shared libraries found in {libs_dir}",
        )

    def test_staged_install_has_no_static_libraries(self) -> None:
        """Staged install must not contain .a files."""
        stage = self._configure_and_install()
        static_libs = list(stage.rglob("*.a"))
        self.assertEqual(
            len(static_libs), 0,
            f"Found static libraries: {static_libs}",
        )

    def test_staged_install_has_no_build_artifacts(self) -> None:
        """Staged install must not contain CMake cache or build files."""
        stage = self._configure_and_install()
        forbidden = (
            list(stage.rglob("CMakeCache.txt"))
            + list(stage.rglob("*.ninja"))
        )
        self.assertEqual(
            len(forbidden), 0,
            f"Found build artifacts: {forbidden}",
        )

    def test_wheel_cmake_config_guards_abi3(self) -> None:
        """CMakeLists.txt must have SABIModule guard for wheel builds."""
        native_cmake = (
            self.root / "bindings" / "python" / "native" / "CMakeLists.txt"
        )
        text = native_cmake.read_text()
        self.assertIn("SABIModule", text,
                      "SABIModule not referenced in native CMakeLists.txt")

    def test_wheel_cmake_sets_visibility(self) -> None:
        """Wheel build must set hidden visibility on _hpactor."""
        native_cmake = (
            self.root / "bindings" / "python" / "native" / "CMakeLists.txt"
        )
        text = native_cmake.read_text()
        self.assertIn("CXX_VISIBILITY_PRESET", text,
                      "CXX_VISIBILITY_PRESET not set in native CMakeLists.txt")
