# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Immutable address and handle types for the HPActor Python binding."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class ActorAddress:
    """Immutable network-addressable actor identity."""

    family: int  # 0=local, 4=IPv4, 6=IPv6
    packed_address: bytes  # 0, 4, or 16 bytes
    port: int
    actor_type: int
    actor_id: int
    incarnation: int

    def __post_init__(self) -> None:
        if self.family not in (0, 4, 6):
            raise ValueError(f"invalid address family: {self.family}")
        expected = {0: 0, 4: 4, 6: 16}[self.family]
        if len(self.packed_address) != expected:
            raise ValueError(
                f"packed_address length {len(self.packed_address)} "
                f"!= {expected} for family {self.family}"
            )


@dataclass(frozen=True, slots=True)
class ActorRef:
    """Handle to a spawned actor carrying its address and name."""

    address: ActorAddress
    name: str = ""
    generation: int = 0


@dataclass(frozen=True, slots=True)
class ScheduleHandle:
    """Opaque handle returned by schedule/cancel_schedule."""

    value: int
