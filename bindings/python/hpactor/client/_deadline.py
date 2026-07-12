# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Monotonic deadline arithmetic shared by HTTP and CLI transports."""

from __future__ import annotations

import math
import time
from dataclasses import dataclass, field
from typing import Callable

from .errors import OperationTimeout


@dataclass(frozen=True, slots=True)
class Deadline:
    """A monotonic deadline that raises ``OperationTimeout`` when exceeded."""

    expires_at: float
    clock: Callable[[], float] = field(
        default=time.monotonic, repr=False, compare=False
    )

    @classmethod
    def after(
        cls, seconds: float, *, clock: Callable[[], float] = time.monotonic
    ) -> "Deadline":
        if not math.isfinite(seconds) or seconds <= 0:
            raise ValueError(
                f"deadline must be positive and finite, got {seconds}"
            )
        return cls(clock() + seconds, clock)

    def remaining(self) -> float:
        value = self.expires_at - self.clock()
        if value <= 0:
            raise OperationTimeout(phase="total")
        return value
