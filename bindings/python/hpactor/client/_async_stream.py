# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Asyncio UDS/TCP stream adapter for HPAC-framed CLI communication."""

from __future__ import annotations

import asyncio
import time
from typing import Optional

from ._deadline import Deadline
from ._hpac import encode_frame, read_frame
from .config import CliClientConfig
from .errors import ConnectionError, OperationTimeout, ProtocolError


class AsyncCliStream:
    """Asyncio socket stream for UDS or TCP CLI communication."""

    def __init__(self, config: CliClientConfig, deadline: Deadline) -> None:
        self._config = config
        self._deadline = deadline
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None

    async def connect(self) -> None:
        remaining = self._deadline.remaining()
        if self._config.uds_path is not None:
            try:
                self._reader, self._writer = await asyncio.wait_for(
                    asyncio.open_unix_connection(self._config.uds_path),
                    timeout=remaining,
                )
            except asyncio.TimeoutError:
                raise OperationTimeout(phase="connect")
            except OSError as exc:
                raise ConnectionError(
                    f"Failed to connect to UDS {self._config.uds_path}"
                ) from exc
        else:
            try:
                self._reader, self._writer = await asyncio.wait_for(
                    asyncio.open_connection(self._config.host, self._config.port),
                    timeout=remaining,
                )
            except asyncio.TimeoutError:
                raise OperationTimeout(phase="connect")
            except OSError as exc:
                raise ConnectionError(
                    f"Failed to connect to {self._config.host}:{self._config.port}"
                ) from exc

    async def write_frame(self, payload: bytes) -> None:
        frame = encode_frame(
            payload, max_payload_bytes=self._config.max_outbound_payload
        )
        await self._write_exact(frame)

    async def read_frame(self) -> bytes:
        return read_frame(
            self._read_exact, max_payload_bytes=self._config.max_inbound_payload
        )

    async def _write_exact(self, data: bytes) -> None:
        if self._writer is None:
            raise ConnectionError("stream not connected")
        remaining = self._deadline.remaining()
        try:
            self._writer.write(data)
            await asyncio.wait_for(self._writer.drain(), timeout=remaining)
        except asyncio.TimeoutError:
            raise OperationTimeout(phase="write")
        except OSError as exc:
            raise ConnectionError("write failed") from exc

    async def _read_exact(self, n: int) -> bytes:
        if self._reader is None:
            raise ConnectionError("stream not connected")
        remaining = self._deadline.remaining()
        try:
            data = await asyncio.wait_for(self._reader.readexactly(n), timeout=remaining)
        except asyncio.TimeoutError:
            raise OperationTimeout(phase="read")
        except asyncio.IncompleteReadError as exc:
            raise ProtocolError("truncated frame: EOF before payload complete") from exc
        except OSError as exc:
            raise ConnectionError("read failed") from exc
        return data

    async def aclose(self) -> None:
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except (OSError, ConnectionError):
                pass
            self._writer = None
            self._reader = None
