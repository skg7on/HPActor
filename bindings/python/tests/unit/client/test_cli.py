# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for sync/async protobuf CLI clients."""

import struct
import unittest
from unittest import mock

from hpactor.client._hpac import encode_frame
from hpactor.client._proto.cli_pb2 import CliCommand, CliResponse
from hpactor.client._proto.cli_messages_pb2 import (
    InspectStateReply,
    KillReply,
    SystemStatsReply,
)
from hpactor.client.cli import CliClient
from hpactor.client.config import CliClientConfig
from hpactor.client.errors import CliCommandError, ProtocolError


def _loopback_config(port: int = 17777) -> CliClientConfig:
    return CliClientConfig(uds_path=None, host="127.0.0.1", port=port)


def _response_frame(response: CliResponse) -> bytes:
    payload = response.SerializeToString(deterministic=True)
    return struct.pack("!4sI", b"HPAC", len(payload)) + payload


class CliClientTest(unittest.TestCase):
    def setUp(self) -> None:
        self._commands: list[CliCommand] = []

    def _make_client(self, response: CliResponse) -> CliClient:
        raw_payload = response.SerializeToString(deterministic=True)
        client = CliClient(_loopback_config())
        # Inject a mock stream that captures commands and returns raw response
        mock_stream = _MockStream(raw_payload, self._commands)
        client._ensure_connected = lambda deadline: mock_stream  # type: ignore[method-assign]
        return client

    def test_execute_serializes_command_and_returns_result(self) -> None:
        resp = CliResponse(
            content_type="text/plain", payload=b"3 actors", is_error=False
        )
        client = self._make_client(resp)
        result = client.execute("actor/list")
        self.assertEqual(result.payload, b"3 actors")
        self.assertEqual(result.content_type, "text/plain")

    def test_execute_preserves_error(self) -> None:
        resp = CliResponse(
            content_type="text/plain",
            payload=b"denied",
            is_error=True,
            error_code=9,
        )
        client = self._make_client(resp)
        with self.assertRaises(CliCommandError) as caught:
            client.execute("actor/7/kill", params={"force": "true"})
        self.assertEqual(caught.exception.error_code, 9)
        self.assertEqual(caught.exception.payload, b"denied")

    def test_structured_inspect_parses_reply(self) -> None:
        expected = InspectStateReply(actor_id=7)
        resp = CliResponse(
            content_type="application/protobuf",
            payload=expected.SerializeToString(deterministic=True),
            is_structured=True,
        )
        client = self._make_client(resp)
        result = client.inspect(7)
        self.assertEqual(result.actor_id, 7)

    def test_structured_kill_parses_reply(self) -> None:
        expected = KillReply(success=True)
        resp = CliResponse(
            content_type="application/protobuf",
            payload=expected.SerializeToString(deterministic=True),
            is_structured=True,
        )
        client = self._make_client(resp)
        result = client.kill(7)
        self.assertTrue(result.success)

    def test_system_stats_parses_reply(self) -> None:
        expected = SystemStatsReply(worker_count=4)
        resp = CliResponse(
            content_type="application/protobuf",
            payload=expected.SerializeToString(deterministic=True),
            is_structured=True,
        )
        client = self._make_client(resp)
        result = client.system_stats()
        self.assertEqual(result.worker_count, 4)

    def test_close_is_idempotent(self) -> None:
        resp = CliResponse(content_type="text/plain", payload=b"ok")
        client = self._make_client(resp)
        client.close()
        client.close()  # Should not raise

    def test_builds_command_path_correctly(self) -> None:
        resp = CliResponse(content_type="text/plain", payload=b"ok")
        client = self._make_client(resp)
        client.execute("system/stats", params={"format": "json"}, args=["verbose"])
        cmd = self._commands[0]
        self.assertEqual(cmd.path, "system/stats")
        self.assertEqual(cmd.params["format"], "json")


class _MockStream:
    """Simulates a blocking HPAC stream for unit tests.

    ``write_frame`` receives raw protobuf bytes (already serialized
    CliCommand) and records them.  ``read_frame`` returns raw protobuf
    bytes (the serialized CliResponse payload without HPAC framing).
    """

    def __init__(self, response_raw: bytes, commands: list[CliCommand]) -> None:
        self._response_raw = response_raw
        self._commands = commands
        self._closed = False

    def write_frame(self, payload: bytes) -> None:
        cmd = CliCommand()
        cmd.ParseFromString(payload)
        self._commands.append(cmd)

    def read_frame(self) -> bytes:
        return self._response_raw

    def close(self) -> None:
        self._closed = True
