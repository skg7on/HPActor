# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Shared immutable value types for the external client SDK."""

from __future__ import annotations

import enum
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Optional, Tuple


class Capability(enum.Enum):
    """Capabilities that a bundle may expose."""

    HEALTH = "health"
    METRICS = "metrics"
    GATEWAY = "gateway"
    CLI = "cli"


class TransportOwnership(enum.Enum):
    """Who owns and is responsible for closing a transport."""

    CLIENT = "client"
    INJECTED = "injected"


class HealthProbe(enum.Enum):
    """Type of health endpoint probe."""

    LIVENESS = "liveness"
    READINESS = "readiness"


class HealthState(enum.Enum):
    """Observed health state."""

    HEALTHY = "healthy"
    DEGRADED = "degraded"
    UNHEALTHY = "unhealthy"
    UNKNOWN = "unknown"

    @classmethod
    def from_wire(cls, value: str) -> "HealthState":
        try:
            return cls(value.lower())
        except ValueError:
            return cls.UNKNOWN


@dataclass(frozen=True, slots=True)
class HealthCheck:
    """A single named health check result."""

    name: str
    status: HealthState
    reason: str = ""


@dataclass(frozen=True, slots=True)
class HealthResult:
    """Parsed health endpoint response."""

    probe: HealthProbe
    state: HealthState
    http_status: int
    checks: tuple[HealthCheck, ...] = ()
    content_type: str = ""
    raw_body: bytes = b""
    elapsed: float = 0.0


@dataclass(frozen=True, slots=True)
class MetricsSnapshot:
    """Raw exposition text from a metrics scrape."""

    text: str
    content_type: str
    http_status: int
    collected_at: datetime
    elapsed: float
    etag: str | None = None


@dataclass(frozen=True, slots=True)
class MetricsNotModified:
    """A conditional metrics scrape returned 304 Not Modified."""

    etag: str | None = None


@dataclass(frozen=True, slots=True)
class CliResult:
    """Result from a command-tree CLI execution."""

    content_type: str
    payload: bytes
    is_structured: bool = False
    is_error: bool = False
    error_code: int = 0


class ResultCategory(enum.Enum):
    """Result category for request events."""

    SUCCESS = "success"
    TRANSPORT_ERROR = "transport_error"
    TIMEOUT = "timeout"
    CLIENT_ERROR = "client_error"
    SERVER_ERROR = "server_error"
    CANCELLED = "cancelled"


@dataclass(frozen=True, slots=True)
class RequestEvent:
    """An observability event emitted after each operation.

    Contains no headers, body, or CLI arguments. Hook errors are isolated
    and logged rather than replacing the operation result.
    """

    capability: str
    operation: str
    origin: str  # redacted base_url or socket path
    attempt: int = 1
    duration: float = 0.0
    request_bytes: int = 0
    response_bytes: int = 0
    category: ResultCategory = ResultCategory.SUCCESS
