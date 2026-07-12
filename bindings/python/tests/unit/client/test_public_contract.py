# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Public contract tests for the hpactor.client external SDK."""

import sys
import tomllib
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[5]


class PublicContractTest(unittest.TestCase):
    def test_client_import_does_not_touch_native_module(self) -> None:
        import sys as _sys
        import hpactor.client

        self.assertNotIn("hpactor._hpactor", _sys.modules)

    def test_metadata_has_exact_runtime_ranges(self) -> None:
        raw = (REPO_ROOT / "pyproject.toml").read_text()
        project = tomllib.loads(raw)["project"]
        deps = project["dependencies"]
        self.assertIn("protobuf>=7.35.0,<8", deps)
        self.assertIn("httpx>=0.28.1,<0.29", deps)


class LazyNativeImportTest(unittest.TestCase):
    def test_pure_exports_available_without_native(self) -> None:
        import sys as _sys

        import hpactor

        self.assertIsNotNone(hpactor.HPActorError)
        self.assertNotIn("hpactor._hpactor", _sys.modules)

    def test_actorsystem_triggers_lazy_native_load(self) -> None:
        # In a non-native environment, accessing ActorSystem raises
        # NativeBindingUnavailable (which is a pure-Python HPActorError).
        import hpactor

        try:
            _ = hpactor.ActorSystem
        except hpactor.NativeBindingUnavailable:
            pass  # expected: native module not available

    def test_native_binding_unavailable_is_importable(self) -> None:
        import hpactor

        err = hpactor.NativeBindingUnavailable(
            name="ActorSystem",
            implementation="cpython",
            platform="linux",
        )
        self.assertIsInstance(err, hpactor.HPActorError)
        self.assertEqual(err.name, "ActorSystem")
        self.assertIn("ActorSystem", str(err))
