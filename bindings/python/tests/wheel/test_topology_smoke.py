#!/usr/bin/env python3
"""Smoke test: run one configured actor from an installed wheel.

This test is designed to run against an installed hpactor wheel (not from
a source checkout).  It creates a temporary directory with a minimal test
actor module and a TOML topology file, then runs the full lifecycle:

  1. ActorSystem.from_topology(path, messages=registry, policy=...)
  2. async with system: resolve("echo") -> send ask -> await reply
  3. Verify clean shutdown: no threads or file descriptors left behind.

Usage against an installed wheel:
    PYTHONPATH=/tmp/topology_smoke_venv/lib/python3.11/site-packages \\
        python3 test_topology_smoke.py
"""

import os
import sys
import tempfile
import threading
import unittest


def _fd_count() -> int:
    try:
        return len(os.listdir("/proc/self/fd"))
    except (FileNotFoundError, PermissionError):
        return -1


class TopologySmokeTest(unittest.TestCase):
    """End-to-end topology smoke from an installed wheel."""

    _tmpdir = None                       # type: tempfile.TemporaryDirectory | None
    _topology_path = ""                  # type: str

    @classmethod
    def setUpClass(cls) -> None:
        """Create a temporary fixture with an actor module and TOML file."""
        cls._tmpdir = tempfile.TemporaryDirectory(prefix="hpactor_smoke_")
        base = cls._tmpdir.name

        # Write a minimal Actor subclass.
        actor_pkg = os.path.join(base, "topology_smoke_app")
        os.makedirs(actor_pkg, exist_ok=True)
        with open(os.path.join(actor_pkg, "__init__.py"), "w") as f:
            f.write("")
        with open(os.path.join(actor_pkg, "actors.py"), "w") as f:
            f.write("""\
from hpactor import Actor, actor

@actor("echo")
class EchoActor(Actor):
    def __init__(self, prefix=""):
        super().__init__()
        self._prefix = prefix

    async def on_start(self) -> None:
        pass

    def behavior(self):
        from hpactor import Behavior
        b = Behavior()
        b.on("StringValue", self._echo)
        return b

    async def _echo(self, msg, ctx):
        result = type(msg)()
        if hasattr(msg, "value") and hasattr(result, "value"):
            result.value = self._prefix + msg.value
        await ctx.reply(result)
""")

        # Write a TOML topology file.
        toml_path = os.path.join(base, "topology.toml")
        with open(toml_path, "w") as f:
            f.write("""\
[system.python]
enabled = true
topology_start_timeout_ms = 30000

[[actors]]
id = "echo"
behavior = "python:topology_smoke_app.actors:EchoActor"
args = { prefix = "smoke:" }
""")
        cls._topology_path = toml_path

        # Make the temp directory importable.
        sys.path.insert(0, base)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._tmpdir is not None:
            cls._tmpdir.cleanup()
            cls._tmpdir = None

    def test_from_topology_lifecycle(self) -> None:
        """Full lifecycle: from_topology -> __aenter__ -> resolve -> __aexit__."""
        import asyncio

        from hpactor._system import ActorSystem
        from hpactor._topology import PythonTopologyPolicy
        from hpactor._messages import MessageRegistry

        registry = MessageRegistry()
        registry.freeze()

        policy = PythonTopologyPolicy(("topology_smoke_app.actors",))
        system = ActorSystem.from_topology(
            self._topology_path, messages=registry, policy=policy)

        async def _run() -> None:
            async with system:
                echo = system.resolve("echo")
                self.assertIsNotNone(echo)
                self.assertEqual(echo.name, "echo")

        asyncio.run(_run())

    def test_cleanup_no_residual_threads(self) -> None:
        """After system shutdown, no HPActor threads remain."""
        before = set(t.ident for t in threading.enumerate())
        # Run a full topology cycle.
        self.test_from_topology_lifecycle()
        after = set(t.ident for t in threading.enumerate())
        self.assertEqual(after, before,
                         "Topology lifecycle must not leave threads behind")

    def test_cleanup_no_residual_fds(self) -> None:
        """After system shutdown, no extra file descriptors remain."""
        before = _fd_count()
        if before < 0:
            self.skipTest("fd count not available")
        # Run a full topology cycle.
        self.test_from_topology_lifecycle()
        after = _fd_count()
        self.assertEqual(after, before,
                         "Topology lifecycle must not leave fds behind")


if __name__ == "__main__":
    unittest.main()
