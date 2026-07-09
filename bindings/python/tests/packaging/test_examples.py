"""Verify that Python binding examples and documentation are complete.

Checks that every example file uses explicit protobuf TypeTags, avoids
JSON/pickle serialization, and that the manual toctree is complete.
"""

import os
import unittest
from pathlib import Path


class PythonExamplesTest(unittest.TestCase):
    """Verify example scripts meet packaging requirements."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())

    def test_examples_directory_exists(self) -> None:
        examples_dir = self.root / "bindings" / "python" / "examples"
        self.assertTrue(examples_dir.is_dir(),
                        f"{examples_dir} not found or not a directory")

    def test_readme_exists(self) -> None:
        readme = self.root / "bindings" / "python" / "examples" / "README.md"
        self.assertTrue(readme.exists(), f"{readme} not found")

    def test_core_examples_exist(self) -> None:
        for name in ("echo.py", "request_response.py",
                     "supervision.py", "operations.py"):
            path = self.root / "bindings" / "python" / "examples" / name
            self.assertTrue(path.exists(), f"Example {name} not found")

    def test_examples_use_explicit_type_tags(self) -> None:
        examples_dir = self.root / "bindings" / "python" / "examples"
        for path in examples_dir.glob("*.py"):
            text = path.read_text()
            self.assertNotIn("pickle", text,
                             f"{path.name} must not use pickle")
            self.assertNotIn("json.dumps(message", text,
                             f"{path.name} must not use JSON for messages")
            self.assertIn("type_tag=0x1000", text,
                          f"{path.name} must use explicit TypeTag")

    def test_examples_have_main_guards(self) -> None:
        examples_dir = self.root / "bindings" / "python" / "examples"
        for path in examples_dir.glob("*.py"):
            text = path.read_text()
            self.assertIn("asyncio.run(main())", text,
                          f"{path.name} must use asyncio.run(main())")


class PythonDocsTest(unittest.TestCase):
    """Verify the Python manual section is complete."""

    def setUp(self) -> None:
        self.root = Path(os.getcwd())

    def test_manual_python_directory_exists(self) -> None:
        manual = self.root / "docs" / "manual" / "python"
        self.assertTrue(manual.is_dir(),
                        f"{manual} not found or not a directory")

    def test_manual_has_complete_python_pages(self) -> None:
        index = self.root / "docs" / "manual" / "python" / "index.rst"
        text = index.read_text()
        for page in (
            "installation",
            "first-actor",
            "message-passing",
            "lifecycle",
            "operations",
            "deployment",
            "api",
        ):
            self.assertIn(page, text,
                          f"python/{page} not in Python manual index")

    def test_all_rst_pages_exist(self) -> None:
        for page in (
            "installation", "first-actor", "message-passing",
            "lifecycle", "operations", "deployment", "api",
        ):
            path = self.root / "docs" / "manual" / "python" / f"{page}.rst"
            self.assertTrue(path.exists(),
                            f"{path} not found")
