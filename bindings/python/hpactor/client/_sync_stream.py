# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Blocking UDS/TCP stream adapter for HPAC-framed CLI communication."""

from __future__ import annotations

import socket
import time
from typing import Optional

from ._deadline import Deadline
from ._hpac import encode_frame, read_frame
from .config import CliClientConfig
from .errors import ConnectionError, OperationTimeout, ProtocolError


class SyncCliStream:
    """Blocking socket stream for UDS or TCP CLI communication."""

    def __init__(self, config: CliClientConfig, deadline: Deadline) -> None:
        self._config = config
        self._deadline = deadline
        self._sock: Optional[socket.socket] = None

    def connect(self) -> None:
        remaining = self._deadline.remaining()
        if self._config.uds_path is not None:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.settimeout(remaining)
            try:
                self._sock.connect(self._config.uds_path)
            except (OSError, socket.timeout) as exc:
                self._sock.close()
                self._sock = None
                raise ConnectionError(
                    f"Failed to connect to UDS {self._config.uds_path}"
                ) from exc
        else:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(remaining)
            try:
                self._sock.connect((self._config.host, self._config.port))
            except (OSError, socket.timeout) as exc:
                self._sock.close()
                self._sock = None
                raise ConnectionError(
                    f"Failed to connect to {self._config.host}:{self._config.port}"
                ) from exc

    def write_frame(self, payload: bytes) -> None:
        frame = encode_frame(payload, max_payload_bytes=self._config.max_outbound_payload)
        self._write_exact(frame)

    def read_frame(self) -> bytes:
        return read_frame(self._read_exact, max_payload_bytes=self._config.max_inbound_payload)

    def _write_exact(self, data: bytes) -> None:
        if self._sock is None:
            raise ConnectionError("stream not connected")
        remaining = self._deadline.remaining()
        self._sock.settimeout(remaining)
        view = memoryview(data)
        while view:
            try:
                sent = self._sock.send(view)
            except (OSError, socket.timeout) as exc:
                raise ConnectionError("write failed") from exc
            if sent == 0:
                raise ConnectionError("write returned 0")
            view = view[sent:]

    def _read_exact(self, n: int) -> bytes:
        if self._sock is None:
            raise ConnectionError("stream not connected")
        remaining = self._deadline.remaining()
        self._sock.settimeout(remaining)
        buf = bytearray()
        while len(buf) < n:
            try:
                chunk = self._sock.recv(n - len(buf))
            except socket.timeout:
                raise OperationTimeout(phase="read")
            except OSError as exc:
                raise ConnectionError("read failed") from exc
            if not chunk:
                raise ProtocolError("truncated frame: EOF before payload complete")
            buf.extend(chunk)
        return bytes(buf)

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
