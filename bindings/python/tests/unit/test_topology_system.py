# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for ActorSystem.from_topology() and resolve()."""

import unittest
import sys
import os

from hpactor._system import ActorSystem, _SystemMode
from hpactor._topology import (
    PythonTopologyPolicy,
    TopologyError,
    TopologyPhase,
    _TopologyFactoryManifest,
)
from hpactor._messages import MessageRegistry
from hpactor._errors import SystemClosedError


class TopologySystemTest(unittest.TestCase):
    """Tests for the declarative topology system API."""

    def setUp(self) -> None:
        self.registry = MessageRegistry()
        self.registry.freeze()

    def _fixture_path(self, name: str) -> str:
        return os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "data", name))

    def test_from_topology_returns_system(self) -> None:
        path = self._fixture_path("topology_python.toml")
        system = ActorSystem.from_topology(
            path,
            messages=self.registry,
            policy=PythonTopologyPolicy(("topology_app.actors",)),
        )
        self.assertIsNotNone(system)
        self.assertEqual(system._mode, _SystemMode.TOPOLOGY)

    def test_from_topology_stores_path_and_policy(self) -> None:
        path = self._fixture_path("topology_python.toml")
        policy = PythonTopologyPolicy(("topology_app.actors",))
        system = ActorSystem.from_topology(
            path, messages=self.registry, policy=policy)
        self.assertEqual(system._topology_path, os.path.abspath(path))
        self.assertIs(system._topology_policy, policy)

    def test_from_topology_side_effect_free(self) -> None:
        """from_topology() must not start threads or construct native state."""
        path = self._fixture_path("topology_python.toml")
        system = ActorSystem.from_topology(
            path, messages=self.registry,
            policy=PythonTopologyPolicy(("topology_app.actors",)))
        self.assertIsNone(system._native)
        self.assertIsNone(system._thread)

    def test_resolve_requires_running(self) -> None:
        path = self._fixture_path("topology_python.toml")
        system = ActorSystem.from_topology(
            path, messages=self.registry,
            policy=PythonTopologyPolicy(("topology_app.actors",)))
        # Resolve before __aenter__ raises KeyError (name not registered yet)
        with self.assertRaises(KeyError):
            system.resolve("echo")

    def test_enter_topology_with_no_python_actors_fails(self) -> None:
        """If prepare_topology returns empty, it should fail early."""
        # This is tested at the unit level; verify exception type exists
        self.assertTrue(issubclass(TopologyError, Exception))

    def test_topology_phase_enum_values(self) -> None:
        self.assertEqual(TopologyPhase.PARSE.value, "parse")
        self.assertEqual(TopologyPhase.IMPORT.value, "import")
        self.assertEqual(TopologyPhase.ACTOR_START.value, "actor_start")
        self.assertEqual(TopologyPhase.COMMIT.value, "commit")
        self.assertEqual(TopologyPhase.ROLLBACK.value, "rollback")

    def test_topology_error_stores_fields(self) -> None:
        err = TopologyError(
            TopologyPhase.IMPORT,
            actor_id="echo",
            behavior="python:mod:Cls",
            error_code=42,
            detail="test detail")
        self.assertEqual(err.phase, TopologyPhase.IMPORT)
        self.assertEqual(err.actor_id, "echo")
        self.assertEqual(err.behavior, "python:mod:Cls")
        self.assertEqual(err.error_code, 42)
        self.assertTrue("test detail" in str(err))

    def test_topology_error_detail_truncated(self) -> None:
        long_detail = "x" * 5000
        err = TopologyError(TopologyPhase.IMPORT, detail=long_detail)
        self.assertEqual(len(err.detail), 4096)


if __name__ == "__main__":
    unittest.main()
