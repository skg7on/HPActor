# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Delivery options, statuses, results, and awaitable receipts."""

from __future__ import annotations

import asyncio
import concurrent.futures
import enum
from dataclasses import dataclass, field
from typing import Optional

from ._address import ActorAddress, ActorRef


class DeliveryMode(enum.IntEnum):
    """Delivery guarantee level."""

    BestEffort = 0
    ObservableBestEffort = 1
    AtLeastOnce = 2
    DurableAtLeastOnce = 3


class DeliveryStatus(enum.IntEnum):
    """Outcome of a tracked delivery."""

    Accepted = 0
    AcceptedWithPressure = 1
    NoRoute = 2
    ActorDead = 3
    MailboxFull = 4
    Expired = 5
    Duplicate = 6
    RemoteUnavailable = 7
    RejectedByPolicy = 8
    SerializationError = 9
    TransportError = 10
    ShuttingDown = 11
    Cancelled = 12


class FailureReason(enum.IntEnum):
    """Canonical delivery/send failure reasons (matches C++ enum)."""

    NoRoute = 0
    NodeUnavailable = 1
    RemoteUnavailable = 2
    NodeQuarantined = 3
    NodeReplaced = 4
    ActorDead = 10
    ActorNotReady = 11
    Quarantined = 12
    CircuitOpen = 13
    MailboxFull = 20
    OutboundQueueFull = 21
    MemoryPressure = 22
    ResourceExhausted = 23
    Expired = 30
    Timeout = 31
    RejectedByPolicy = 40
    Dropped = 41
    MailboxClosed = 42
    SerializationError = 50
    TransportError = 51
    FrameRejected = 52
    FencingTokenStale = 53
    Duplicate = 60
    Draining = 70
    ShuttingDown = 71
    RetryExhausted = 80
    Cancelled = 81
    SpawnFailed = 90
    PassivationDrainTimeout = 100
    PassivationSnapshotFailed = 101
    Unknown = 255


class FailureSource(enum.IntEnum):
    """Subsystem that produced a failure."""

    ActorRuntime = 0
    Mailbox = 1
    Rpc = 2
    Transport = 3
    Discovery = 4
    Scheduler = 5
    Config = 6
    Security = 7
    DurableStore = 8
    Supervision = 9
    Cluster = 10
    Unknown = 11
    LanguageBinding = 12


@dataclass(frozen=True, slots=True)
class DeliveryOptions:
    """Options controlling send semantics."""

    delivery_mode: DeliveryMode = DeliveryMode.BestEffort
    no_drop: bool = False
    emit_backpressure: bool = False
    priority: int = 0
    deadline_ns: int = 0
    message_id: int = 0
    flags: int = 0

    def __post_init__(self) -> None:
        if not (0 <= self.priority <= 3):
            raise ValueError("priority must be 0..3")


@dataclass(frozen=True, slots=True)
class DeliveryResult:
    """Immutable outcome of a tracked delivery."""

    status: DeliveryStatus = DeliveryStatus.Accepted
    target: Optional[ActorRef] = None
    message_id: int = 0
    detail_code: int = 0
    retry_after_ns: int = -1

    @property
    def accepted(self) -> bool:
        return self.status in (
            DeliveryStatus.Accepted,
            DeliveryStatus.AcceptedWithPressure,
        )

    @property
    def retryable(self) -> bool:
        return self.status in (
            DeliveryStatus.NoRoute,
            DeliveryStatus.ActorDead,
            DeliveryStatus.MailboxFull,
            DeliveryStatus.RemoteUnavailable,
            DeliveryStatus.TransportError,
        )

    @property
    def failure_reason(self) -> FailureReason:
        """Map delivery status to the canonical FailureReason."""
        _status_to_reason = {
            DeliveryStatus.Accepted: FailureReason.Unknown,
            DeliveryStatus.AcceptedWithPressure: FailureReason.MemoryPressure,
            DeliveryStatus.NoRoute: FailureReason.NoRoute,
            DeliveryStatus.ActorDead: FailureReason.ActorDead,
            DeliveryStatus.MailboxFull: FailureReason.MailboxFull,
            DeliveryStatus.Expired: FailureReason.Expired,
            DeliveryStatus.Duplicate: FailureReason.Duplicate,
            DeliveryStatus.RemoteUnavailable: FailureReason.RemoteUnavailable,
            DeliveryStatus.RejectedByPolicy: FailureReason.RejectedByPolicy,
            DeliveryStatus.SerializationError: FailureReason.SerializationError,
            DeliveryStatus.TransportError: FailureReason.TransportError,
            DeliveryStatus.ShuttingDown: FailureReason.ShuttingDown,
            DeliveryStatus.Cancelled: FailureReason.Cancelled,
        }
        return _status_to_reason.get(self.status, FailureReason.Unknown)


class DeliveryReceipt:
    """Awaitable handle for the eventual outcome of a tracked delivery.

    Stores a thread-safe concurrent.futures.Future. ``await receipt``
    shields an asyncio wrapper so cancellation of one waiter does not
    cancel native retry work or the shared future.
    """

    def __init__(self) -> None:
        self._future: concurrent.futures.Future[DeliveryResult] = (
            concurrent.futures.Future()
        )

    def __await__(self):
        # Shield: cancellation only affects the asyncio wrapper, not the
        # underlying concurrent future (native retries continue).
        loop = asyncio.get_running_loop()
        wrapped = asyncio.wrap_future(self._future, loop=loop)
        return wrapped.__await__()

    def done(self) -> bool:
        return self._future.done()

    def result(self) -> DeliveryResult:
        return self._future.result()

    def _resolve(self, result: DeliveryResult) -> None:
        """Internal: resolve the receipt (called from runtime)."""
        if not self._future.done():
            self._future.set_result(result)
