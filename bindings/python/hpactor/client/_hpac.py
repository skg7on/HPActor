# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Pure-Python HPAC frame encoder and decoder.

Wire format::

    bytes 0..3   ASCII "HPAC"
    bytes 4..7   unsigned payload length in network byte order
    bytes 8..N   protobuf payload

This is byte-for-byte compatible with the C++ implementation in
``src/cli/command_utils.cpp``.
"""

from __future__ import annotations

import struct
from typing import Callable

from .errors import ProtocolError, ResponseLimitError

_HEADER = struct.Struct("!4sI")
_MAGIC = b"HPAC"


def encode_frame(payload: bytes, *, max_payload_bytes: int) -> bytes:
    """Encode *payload* as an HPAC frame.

    Raises ``ResponseLimitError`` if the payload exceeds *max_payload_bytes*.
    """
    size = len(payload)
    if size > max_payload_bytes:
        raise ResponseLimitError(
            limit=max_payload_bytes,
            observed=size,
            resource="cli outbound frame",
        )
    return _HEADER.pack(_MAGIC, size) + payload


def read_frame(
    read_exact: Callable[[int], bytes],
    *,
    max_payload_bytes: int,
) -> bytes:
    """Read one HPAC frame from the *read_exact* callback.

    *read_exact(n)* must return exactly *n* bytes or raise ``EOFError``.

    Raises ``ProtocolError`` for invalid magic or truncated reads.
    Raises ``ResponseLimitError`` if the declared payload length exceeds
    *max_payload_bytes*.
    """
    header = read_exact(_HEADER.size)
    if len(header) < _HEADER.size:
        raise ProtocolError("truncated HPAC frame header")
    magic, size = _HEADER.unpack(header)
    if magic != _MAGIC:
        raise ProtocolError(
            f"invalid HPAC magic: expected {_MAGIC!r}, got {magic!r}"
        )
    if size > max_payload_bytes:
        raise ResponseLimitError(
            limit=max_payload_bytes,
            observed=size,
            resource="cli inbound frame",
        )
    payload = read_exact(size)
    if len(payload) < size:
        raise ProtocolError("truncated HPAC frame payload")
    return payload
