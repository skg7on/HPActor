# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for the pure-Python HPAC frame codec."""

import struct
import unittest

from hpactor.client._hpac import encode_frame, read_frame
from hpactor.client.errors import ProtocolError, ResponseLimitError

MAGIC = b"HPAC"


class HpacCodecTest(unittest.TestCase):
    def test_encode_produces_correct_header(self) -> None:
        payload = b"hello"
        encoded = encode_frame(payload, max_payload_bytes=1024)
        self.assertEqual(encoded[:4], MAGIC)
        self.assertEqual(encoded[4:8], struct.pack("!I", len(payload)))
        self.assertEqual(encoded[8:], payload)

    def test_encode_empty_payload(self) -> None:
        encoded = encode_frame(b"", max_payload_bytes=1024)
        self.assertEqual(encoded[:4], MAGIC)
        self.assertEqual(encoded[4:8], struct.pack("!I", 0))
        self.assertEqual(len(encoded), 8)

    def test_encode_oversized_payload_raises(self) -> None:
        with self.assertRaises(ResponseLimitError) as caught:
            encode_frame(b"x" * 17, max_payload_bytes=16)
        self.assertEqual(caught.exception.limit, 16)
        self.assertEqual(caught.exception.resource, "cli outbound frame")

    def test_encode_at_limit_payload_succeeds(self) -> None:
        encoded = encode_frame(b"x" * 16, max_payload_bytes=16)
        self.assertEqual(len(encoded), 8 + 16)

    def test_read_frame_returns_payload(self) -> None:
        payload = b"response-data"
        header = MAGIC + struct.pack("!I", len(payload))
        frame = header + payload

        def read_exact(n: int) -> bytes:
            nonlocal frame
            data, frame = frame[:n], frame[n:]
            return data

        result = read_frame(read_exact, max_payload_bytes=1024)
        self.assertEqual(result, payload)

    def test_read_frame_rejects_invalid_magic(self) -> None:
        bad_header = b"XXXX" + struct.pack("!I", 5)
        pos = [0]

        def read_exact(n: int) -> bytes:
            data = bad_header[pos[0]:pos[0] + n]
            pos[0] += n
            return data

        with self.assertRaises(ProtocolError) as caught:
            read_frame(read_exact, max_payload_bytes=1024)
        self.assertIn("magic", str(caught.exception))

    def test_read_frame_rejects_oversized_declared_length(self) -> None:
        header = MAGIC + struct.pack("!I", 17)
        pos = [0]

        def read_exact(n: int) -> bytes:
            data = header[pos[0]:pos[0] + n]
            pos[0] += n
            return data

        with self.assertRaises(ResponseLimitError) as caught:
            read_frame(read_exact, max_payload_bytes=16)
        self.assertEqual(caught.exception.limit, 16)
        self.assertEqual(caught.exception.observed, 17)

    def test_read_frame_at_limit_succeeds(self) -> None:
        payload = b"a" * 16
        header = MAGIC + struct.pack("!I", len(payload))
        frame = header + payload

        def read_exact(n: int) -> bytes:
            nonlocal frame
            data, frame = frame[:n], frame[n:]
            return data

        result = read_frame(read_exact, max_payload_bytes=16)
        self.assertEqual(result, payload)

    def test_magic_bytes_match_cpp(self) -> None:
        self.assertEqual(MAGIC, b"HPAC")
