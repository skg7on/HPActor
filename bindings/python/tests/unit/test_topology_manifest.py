# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for _TopologyFactoryManifest preflight and validation."""

import unittest

from hpactor._topology import (
    PythonTopologyPolicy,
    TopologyError,
    TopologyPhase,
    _TopologyFactoryManifest,
)
from hpactor._messages import MessageRegistry


class TopologyManifestTest(unittest.TestCase):
    """Tests for factory manifest import and class validation."""

    def setUp(self) -> None:
        self.registry = MessageRegistry()

    def _make_descriptor(self, module: str, qualname: str,
                         index: int = 0, actor_id: str = "test",
                         behavior: str = "", args: tuple = (),
                         fingerprint: int = 0):
        """Build a descriptor tuple matching native prepare_topology output."""
        return (
            index, actor_id,
            behavior or f"python:{module}:{qualname}",
            module, qualname, args, fingerprint,
        )

    def test_policy_rejects_module_not_allowed(self) -> None:
        """Verify that modules outside the allowlist are rejected."""
        policy = PythonTopologyPolicy(("allowed.only",))
        # The module is not in allowed_module_prefixes.
        self.assertFalse(policy.allows("disallowed.module"))
        self.assertTrue(policy.allows("allowed.only"))

    def test_policy_allows_exact_prefix(self) -> None:
        policy = PythonTopologyPolicy(("topology_app.actors",))
        self.assertTrue(policy.allows("topology_app.actors"))

    def test_policy_allows_child_module(self) -> None:
        policy = PythonTopologyPolicy(("topology_app",))
        self.assertTrue(policy.allows("topology_app.actors"))

    def test_manifest_starts_unfrozen(self) -> None:
        manifest = _TopologyFactoryManifest()
        self.assertFalse(manifest.frozen)

    def test_manifest_record_lookup_by_token(self) -> None:
        manifest = _TopologyFactoryManifest()
        # Can't easily test preflight without async, but verify no records yet
        with self.assertRaises(KeyError):
            manifest.record_for_token(1)

    def test_manifest_token_for_missing_index(self) -> None:
        manifest = _TopologyFactoryManifest()
        with self.assertRaises(KeyError):
            manifest.token_for(0)

    def test_policy_rejects_empty_prefixes(self) -> None:
        with self.assertRaises(ValueError):
            PythonTopologyPolicy(())


if __name__ == "__main__":
    unittest.main()
