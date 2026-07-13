# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Sync and async protobuf CLI clients with HPAC framing."""

from __future__ import annotations

import asyncio
import threading
import time
from typing import Any, Optional

from google.protobuf.message import DecodeError, Message

from ._async_stream import AsyncCliStream
from ._deadline import Deadline
from ._hpac import encode_frame, read_frame
from ._proto.cli_pb2 import CliCommand, CliResponse
from ._proto.cli_messages_pb2 import (
    InspectStateReply,
    KillReply,
    ListActorsReply,
    MemoryStatsReply,
    QuarantineReply,
    SystemStatsReply,
)
from ._sync_stream import SyncCliStream
from .config import CliClientConfig
from .errors import (
    ClientClosedError,
    CliCommandError,
    ConnectionError,
    OperationTimeout,
    ProtocolError,
)
from .models import CliResult

def _build_command(
    path: str | None = None,
    params: dict[str, str] | None = None,
    args: tuple[str, ...] | None = None,
    fmt: str = "pretty",
) -> CliCommand:
    cmd = CliCommand()
    if path is not None:
        cmd.path = path
    if params:
        for k, v in params.items():
            cmd.params[k] = v
    if args:
        cmd.args.extend(args)
    cmd.format = fmt
    return cmd


def _build_rpc_command(method: str, request: Message) -> CliCommand:
    cmd = CliCommand()
    cmd.rpc_method = method
    cmd.rpc_request = request.SerializeToString(deterministic=True)
    return cmd


def _decode_response(raw: bytes) -> CliResponse:
    response = CliResponse()
    try:
        response.ParseFromString(raw)
    except DecodeError as exc:
        raise ProtocolError("invalid CliResponse protobuf") from exc
    return response


def _truncated_payload(payload: bytes) -> bytes:
    return payload[:4096]


class CliClient:
    """Synchronous protobuf CLI client over UDS or TCP."""

    def __init__(self, config: CliClientConfig) -> None:
        self._config = config
        self._stream: Optional[SyncCliStream] = None
        self._lock = threading.Lock()

    def connect(self) -> None:
        """Explicitly create and connect the stream."""
        deadline = Deadline.after(self._config.connect_timeout)
        stream = SyncCliStream(self._config, deadline)
        try:
            stream.connect()
        except BaseException:
            stream.close()
            raise
        self._stream = stream

    def execute(
        self,
        path: str,
        *,
        params: dict[str, str] | None = None,
        args: tuple[str, ...] | None = None,
        fmt: str = "pretty",
    ) -> CliResult:
        """Execute a command-tree path and return a ``CliResult``.

        Raises ``CliCommandError`` if the server returns ``is_error=True``.
        """
        command = _build_command(path=path, params=params, args=args, fmt=fmt)
        response = self._exchange(command)
        if response.is_error:
            raise CliCommandError(
                error_code=response.error_code,
                payload=_truncated_payload(response.payload),
                content_type=response.content_type,
            )
        return CliResult(
            content_type=response.content_type,
            payload=response.payload,
            is_structured=response.is_structured,
            is_error=response.is_error,
            error_code=response.error_code,
        )

    # -- Structured RPC methods ------------------------------------------

    def inspect(self, actor_id: int) -> InspectStateReply:
        from ._proto.cli_messages_pb2 import InspectStateRequest
        req = InspectStateRequest(target_actor_id=actor_id)
        cmd = _build_rpc_command("inspect", req)
        return self._call_structured(cmd, InspectStateReply)

    def kill(self, actor_id: int, *, force: bool = False) -> KillReply:
        from ._proto.cli_messages_pb2 import KillRequest
        req = KillRequest(target_actor_id=actor_id, force=force)
        cmd = _build_rpc_command("kill", req)
        return self._call_structured(cmd, KillReply)

    def quarantine(self, actor_id: int, reason: str = "") -> QuarantineReply:
        from ._proto.cli_messages_pb2 import QuarantineRequest
        req = QuarantineRequest(target_actor_id=actor_id, reason=reason)
        cmd = _build_rpc_command("quarantine", req)
        return self._call_structured(cmd, QuarantineReply)

    def unquarantine(self, actor_id: int) -> QuarantineReply:
        from ._proto.cli_messages_pb2 import QuarantineRequest
        req = QuarantineRequest(target_actor_id=actor_id, unquarantine=True)
        cmd = _build_rpc_command("quarantine", req)
        return self._call_structured(cmd, QuarantineReply)

    def list_actors(
        self,
        shard_index: int = 0,
        offset: int = 0,
        limit: int = 100,
        filter_str: str = "",
    ) -> ListActorsReply:
        from ._proto.cli_messages_pb2 import ListActorsRequest
        req = ListActorsRequest(
            shard_index=shard_index,
            offset=offset,
            limit=limit,
            filter=filter_str,
        )
        cmd = _build_rpc_command("enumerate", req)
        return self._call_structured(cmd, ListActorsReply)

    def system_stats(self) -> SystemStatsReply:
        from ._proto.cli_messages_pb2 import SystemStatsRequest
        req = SystemStatsRequest()
        cmd = _build_rpc_command("system_stats", req)
        return self._call_structured(cmd, SystemStatsReply)

    def memory_stats(self, actor_id: int = 0) -> MemoryStatsReply:
        from ._proto.cli_messages_pb2 import MemoryStatsRequest
        req = MemoryStatsRequest(actor_id=actor_id)
        cmd = _build_rpc_command("memory_stats", req)
        return self._call_structured(cmd, MemoryStatsReply)

    def call(self, rpc_method: str, request: Message, response_type: type[Message]) -> Message:
        cmd = _build_rpc_command(rpc_method, request)
        return self._call_structured(cmd, response_type)

    # -- Internals -------------------------------------------------------

    def _call_structured(self, command: CliCommand, reply_type: type[Message]) -> Any:
        response = self._exchange(command)
        if response.is_error:
            raise CliCommandError(
                error_code=response.error_code,
                payload=_truncated_payload(response.payload),
                content_type=response.content_type,
            )
        if not response.is_structured:
            raise ProtocolError(
                f"expected structured response for RPC {command.rpc_method}"
            )
        reply = reply_type()
        try:
            reply.ParseFromString(response.payload)
        except DecodeError as exc:
            raise ProtocolError(
                f"invalid {reply_type.DESCRIPTOR.name} protobuf"
            ) from exc
        return reply

    def _exchange(self, command: CliCommand) -> CliResponse:
        deadline = Deadline.after(self._config.request_timeout)
        if not self._lock.acquire(timeout=deadline.remaining()):
            raise OperationTimeout(phase="cli request lock")
        try:
            stream = self._ensure_connected(deadline)
            try:
                payload = command.SerializeToString(deterministic=True)
                stream.write_frame(payload)
                raw = stream.read_frame()
                return _decode_response(raw)
            except Exception:
                stream.close()
                self._stream = None
                raise
        finally:
            self._lock.release()

    def _ensure_connected(self, deadline: Deadline) -> SyncCliStream:
        if self._stream is not None:
            return self._stream
        stream = SyncCliStream(self._config, deadline)
        try:
            stream.connect()
        except BaseException:
            stream.close()
            raise
        self._stream = stream
        return stream

    def close(self) -> None:
        if self._stream is not None:
            self._stream.close()
            self._stream = None


class AsyncCliClient:
    """Asynchronous protobuf CLI client over UDS or TCP."""

    def __init__(self, config: CliClientConfig) -> None:
        self._config = config
        self._stream: Optional[AsyncCliStream] = None
        self._lock = asyncio.Lock()

    async def connect(self) -> None:
        deadline = Deadline.after(self._config.connect_timeout)
        stream = AsyncCliStream(self._config, deadline)
        try:
            await stream.connect()
        except BaseException:
            await stream.aclose()
            raise
        self._stream = stream

    async def execute(
        self,
        path: str,
        *,
        params: dict[str, str] | None = None,
        args: tuple[str, ...] | None = None,
        fmt: str = "pretty",
    ) -> CliResult:
        command = _build_command(path=path, params=params, args=args, fmt=fmt)
        response = await self._exchange(command)
        if response.is_error:
            raise CliCommandError(
                error_code=response.error_code,
                payload=_truncated_payload(response.payload),
                content_type=response.content_type,
            )
        return CliResult(
            content_type=response.content_type,
            payload=response.payload,
            is_structured=response.is_structured,
            is_error=response.is_error,
            error_code=response.error_code,
        )

    async def inspect(self, actor_id: int) -> InspectStateReply:
        from ._proto.cli_messages_pb2 import InspectStateRequest
        req = InspectStateRequest(target_actor_id=actor_id)
        cmd = _build_rpc_command("inspect", req)
        return await self._call_structured(cmd, InspectStateReply)

    async def kill(self, actor_id: int, *, force: bool = False) -> KillReply:
        from ._proto.cli_messages_pb2 import KillRequest
        req = KillRequest(target_actor_id=actor_id, force=force)
        cmd = _build_rpc_command("kill", req)
        return await self._call_structured(cmd, KillReply)

    async def system_stats(self) -> SystemStatsReply:
        from ._proto.cli_messages_pb2 import SystemStatsRequest
        req = SystemStatsRequest()
        cmd = _build_rpc_command("system_stats", req)
        return await self._call_structured(cmd, SystemStatsReply)

    async def quarantine(self, actor_id: int, reason: str = "") -> QuarantineReply:
        from ._proto.cli_messages_pb2 import QuarantineRequest
        req = QuarantineRequest(target_actor_id=actor_id, reason=reason)
        cmd = _build_rpc_command("quarantine", req)
        return await self._call_structured(cmd, QuarantineReply)

    async def unquarantine(self, actor_id: int) -> QuarantineReply:
        from ._proto.cli_messages_pb2 import QuarantineRequest
        req = QuarantineRequest(target_actor_id=actor_id, unquarantine=True)
        cmd = _build_rpc_command("quarantine", req)
        return await self._call_structured(cmd, QuarantineReply)

    async def list_actors(
        self,
        shard_index: int = 0,
        offset: int = 0,
        limit: int = 100,
        filter_str: str = "",
    ) -> ListActorsReply:
        from ._proto.cli_messages_pb2 import ListActorsRequest
        req = ListActorsRequest(
            shard_index=shard_index,
            offset=offset,
            limit=limit,
            filter=filter_str,
        )
        cmd = _build_rpc_command("enumerate", req)
        return await self._call_structured(cmd, ListActorsReply)

    async def memory_stats(self, actor_id: int = 0) -> MemoryStatsReply:
        from ._proto.cli_messages_pb2 import MemoryStatsRequest
        req = MemoryStatsRequest(actor_id=actor_id)
        cmd = _build_rpc_command("memory_stats", req)
        return await self._call_structured(cmd, MemoryStatsReply)

    async def call(self, rpc_method: str, request: Message, response_type: type[Message]) -> Message:
        cmd = _build_rpc_command(rpc_method, request)
        return await self._call_structured(cmd, response_type)

    async def _call_structured(
        self, command: CliCommand, reply_type: type[Message]
    ) -> Any:
        response = await self._exchange(command)
        if response.is_error:
            raise CliCommandError(
                error_code=response.error_code,
                payload=_truncated_payload(response.payload),
                content_type=response.content_type,
            )
        if not response.is_structured:
            raise ProtocolError(
                f"expected structured response for RPC {command.rpc_method}"
            )
        reply = reply_type()
        try:
            reply.ParseFromString(response.payload)
        except DecodeError as exc:
            raise ProtocolError(
                f"invalid {reply_type.DESCRIPTOR.name} protobuf"
            ) from exc
        return reply

    async def _exchange(self, command: CliCommand) -> CliResponse:
        deadline = Deadline.after(self._config.request_timeout)
        async with asyncio.timeout(deadline.remaining()):
            async with self._lock:
                stream = await self._ensure_connected(deadline)
                try:
                    payload = command.SerializeToString(deterministic=True)
                    await stream.write_frame(payload)
                    raw = await stream.read_frame()
                    return _decode_response(raw)
                except BaseException:
                    await stream.aclose()
                    self._stream = None
                    raise

    async def _ensure_connected(self, deadline: Deadline) -> AsyncCliStream:
        if self._stream is not None:
            return self._stream
        stream = AsyncCliStream(self._config, deadline)
        try:
            await stream.connect()
        except BaseException:
            await stream.aclose()
            raise
        self._stream = stream
        return stream

    async def aclose(self) -> None:
        if self._stream is not None:
            await self._stream.aclose()
            self._stream = None
