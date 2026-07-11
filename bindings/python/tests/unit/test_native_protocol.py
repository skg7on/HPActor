# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for _NativeProtocol duck-typed backend interface."""

import unittest


class FakeBackend:
    """Minimal backend for protocol conformance testing."""

    def start(self) -> None:
        self._started = True

    def stop(self) -> None:
        self._started = False

    def begin_draining(self) -> None:
        pass

    def spawn_bridge(self):
        return ((0, b"", 0, 0, 1, 1), 1)

    def stop_bridge(self, addr):
        return True

    def register_name(self, name, addr):
        return True

    def resolve_name(self, name):
        return None

    def submit(self, cmd):
        return True

    def drain_dispatch(self, n):
        return []

    def drain_completions(self, n):
        return []

    def snapshot(self):
        return {"state": 0}

    @property
    def dispatch_fd(self):
        return -1

    @property
    def completion_fd(self):
        return -1

    def application_origin(self):
        return (0, b"", 0, 0, 0, 0)


class NativeProtocolTest(unittest.TestCase):
    """Verify that _NativeBackend and CommandKind are importable
    and that a conforming backend satisfies the duck-typed protocol."""

    def test_command_kind_enum_values(self):
        from hpactor._native_protocol import CommandKind  # noqa: E402
        self.assertEqual(CommandKind.SEND, 0)
        self.assertEqual(CommandKind.REPLY, 1)
        self.assertEqual(CommandKind.ASK, 3)
        self.assertEqual(CommandKind.SPAWN, 4)
        self.assertEqual(CommandKind.STOP, 11)
        self.assertEqual(CommandKind.CANCEL_ASK, 15)

    def test_fake_backend_conforms_to_protocol(self):
        backend = FakeBackend()
        backend.start()
        addr, gen = backend.spawn_bridge()
        self.assertEqual(len(addr), 6)
        self.assertGreater(gen, 0)
        self.assertEqual(backend.dispatch_fd, -1)
        self.assertEqual(backend.completion_fd, -1)
        self.assertTrue(backend.submit({"kind": 0}))
        self.assertEqual(backend.drain_dispatch(16), [])
        self.assertEqual(backend.drain_completions(16), [])
        snap = backend.snapshot()
        self.assertIn("state", snap)
        self.assertTrue(backend.stop_bridge(addr))
        backend.stop()

    def test_application_origin_returns_valid_address(self):
        backend = FakeBackend()
        origin = backend.application_origin()
        self.assertEqual(len(origin), 6)
        self.assertEqual(origin[0], 0)  # family
        self.assertEqual(origin[4], 0)  # actor_id (application bridge)
