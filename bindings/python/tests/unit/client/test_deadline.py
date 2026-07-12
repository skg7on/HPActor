# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for monotonic deadline arithmetic."""

import time
import unittest

from hpactor.client._deadline import Deadline
from hpactor.client.errors import OperationTimeout


class DeadlineTest(unittest.TestCase):
    def test_deadline_after_positive_duration(self) -> None:
        dl = Deadline.after(5.0, clock=time.monotonic)
        self.assertGreater(dl.expires_at, 0)
        self.assertGreater(dl.remaining(), 0)

    def test_deadline_zero_raises(self) -> None:
        with self.assertRaises(ValueError):
            Deadline.after(0)

    def test_deadline_negative_raises(self) -> None:
        with self.assertRaises(ValueError):
            Deadline.after(-1.0)

    def test_deadline_infinite_raises(self) -> None:
        with self.assertRaises(ValueError):
            Deadline.after(float("inf"))

    def test_deadline_nan_raises(self) -> None:
        with self.assertRaises(ValueError):
            Deadline.after(float("nan"))

    def test_remaining_returns_finite_seconds(self) -> None:
        clock = FakeClock(1000.0)
        dl = Deadline.after(5.0, clock=clock.now)
        clock.advance(1.0)
        remaining = dl.remaining()
        self.assertAlmostEqual(remaining, 4.0, places=6)

    def test_expired_raises_operation_timeout(self) -> None:
        clock = FakeClock(1000.0)
        dl = Deadline.after(5.0, clock=clock.now)
        clock.advance(6.0)
        with self.assertRaises(OperationTimeout) as caught:
            dl.remaining()
        self.assertEqual(caught.exception.phase, "total")

    def test_exactly_expired_raises(self) -> None:
        clock = FakeClock(1000.0)
        dl = Deadline.after(5.0, clock=clock.now)
        clock.advance(5.0)
        with self.assertRaises(OperationTimeout):
            dl.remaining()


class FakeClock:
    def __init__(self, start: float) -> None:
        self._t = start

    def advance(self, seconds: float) -> None:
        self._t += seconds

    def now(self) -> float:
        return self._t
