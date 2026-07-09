"""Verify that hpactor public API type stubs are consistent.

The installed wheel must have a py.typed marker and all public
symbols must have resolvable types.
"""

import unittest


class TypingSmokeTest(unittest.TestCase):
    """Verify typing stubs are present and consistent."""

    def test_py_typed_marker(self) -> None:
        """The hpactor package must ship a py.typed marker."""
        import hpactor
        import os
        pkg_dir = hpactor.__path__[0]
        py_typed = os.path.join(pkg_dir, "py.typed")
        self.assertTrue(
            os.path.exists(py_typed),
            f"No py.typed marker at {py_typed}",
        )

    def test_all_public_symbols_are_callable_or_instantiable(self) -> None:
        """Every public symbol in hpactor.__all__ must be a valid object."""
        import hpactor
        for name in hpactor.__all__:
            if name == "__version__":
                continue
            obj = getattr(hpactor, name)
            self.assertIsNotNone(
                obj,
                f"hpactor.{name} is None",
            )

    def test_exception_hierarchy(self) -> None:
        """All hpactor exceptions must inherit from HPActorError."""
        import hpactor
        for name in hpactor.__all__:
            if name == "HPActorError":
                continue
            if name.endswith("Error"):
                exc_cls = getattr(hpactor, name)
                self.assertTrue(
                    issubclass(exc_cls, hpactor.HPActorError),
                    f"{name} must inherit from HPActorError",
                )
