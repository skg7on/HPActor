# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Frozen, validated configuration types for the external SDK."""

from __future__ import annotations

import math
import ssl
import urllib.parse
from dataclasses import dataclass, field
from typing import Tuple

from .errors import ConfigurationError, InsecureTransportError

_LOOPBACK_HOSTS = frozenset({"127.0.0.1", "::1", "localhost"})
_MAX_RESPONSE_BYTES = 16 * 1024 * 1024  # 16 MiB


@dataclass(frozen=True, slots=True)
class HttpTimeouts:
    """Connection and read/write timeouts for HTTP requests (seconds)."""

    connect: float = 5.0
    read: float = 30.0
    write: float = 30.0
    pool: float = 5.0

    def __post_init__(self) -> None:
        for name in ("connect", "read", "write", "pool"):
            value = getattr(self, name)
            if not math.isfinite(value) or value <= 0:
                raise ValueError(
                    f"HttpTimeouts.{name} must be positive and finite, got {value}"
                )


@dataclass(frozen=True, slots=True)
class HttpLimits:
    """Connection pool and response body limits."""

    max_keepalive_connections: int = 20
    max_connections: int = 100
    max_response_bytes: int = _MAX_RESPONSE_BYTES

    def __post_init__(self) -> None:
        for name in (
            "max_keepalive_connections",
            "max_connections",
            "max_response_bytes",
        ):
            value = getattr(self, name)
            if not isinstance(value, int) or value <= 0:
                raise ValueError(
                    f"HttpLimits.{name} must be a positive integer, got {value}"
                )


@dataclass(frozen=True, slots=True)
class RetryPolicy:
    """Opt-in bounded retry for idempotent HTTP requests only."""

    attempts: int = 1
    initial_delay: float = 0.1
    max_delay: float = 2.0
    retry_statuses: frozenset[int] = frozenset({429, 502, 503, 504})

    def __post_init__(self) -> None:
        if not 1 <= self.attempts <= 3:
            raise ValueError(
                f"RetryPolicy.attempts must be in 1..3, got {self.attempts}"
            )
        if not math.isfinite(self.initial_delay) or self.initial_delay <= 0:
            raise ValueError(
                f"RetryPolicy.initial_delay must be positive and finite, "
                f"got {self.initial_delay}"
            )
        if not math.isfinite(self.max_delay) or self.max_delay <= 0:
            raise ValueError(
                f"RetryPolicy.max_delay must be positive and finite, "
                f"got {self.max_delay}"
            )


@dataclass(frozen=True, slots=True)
class HttpEndpointConfig:
    """Configuration for an HTTP(S) endpoint."""

    base_url: str
    headers: tuple[tuple[str, str], ...] = ()
    verify: bool | ssl.SSLContext = True
    cert: str | tuple[str, str] | None = None
    follow_redirects: bool = False
    timeouts: HttpTimeouts = field(default_factory=HttpTimeouts)
    limits: HttpLimits = field(default_factory=HttpLimits)
    retry: RetryPolicy = field(default_factory=RetryPolicy)

    def __post_init__(self) -> None:
        parsed = urllib.parse.urlparse(self.base_url)
        if parsed.scheme not in ("http", "https"):
            raise ValueError(
                f"HttpEndpointConfig.base_url must use http or https scheme, "
                f"got {parsed.scheme!r}"
            )
        if parsed.username or parsed.password:
            raise ValueError(
                "HttpEndpointConfig.base_url must not contain userinfo "
                "(credentials in URL)"
            )
        if parsed.fragment:
            raise ValueError(
                "HttpEndpointConfig.base_url must not contain a fragment"
            )
        self._validate_headers(self.headers)

    @staticmethod
    def _validate_headers(headers: tuple[tuple[str, str], ...]) -> None:
        for name, value in headers:
            if "\r" in name or "\n" in name or "\r" in value or "\n" in value:
                raise ValueError(
                    f"Header {name!r}: value must not contain CR or LF"
                )

    def __repr__(self) -> str:
        safe_headers = tuple(
            (n, _redact_header_value(n, v)) for n, v in self.headers
        )
        return (
            f"HttpEndpointConfig(base_url={self.base_url!r}, "
            f"headers={safe_headers}, verify={self.verify!r}, "
            f"cert={'***' if self.cert else None}, "
            f"follow_redirects={self.follow_redirects}, "
            f"timeouts={self.timeouts!r}, limits={self.limits!r}, "
            f"retry={self.retry!r})"
        )


def _redact_header_value(name: str, value: str) -> str:
    name_lower = name.lower()
    if name_lower in frozenset({
        "authorization", "proxy-authorization", "cookie", "set-cookie",
    }):
        return "***"
    return value


def _validate_path(path: str, *, allow_empty: bool = False) -> None:
    if not path:
        if not allow_empty:
            raise ValueError("path must not be empty")
        return
    if not path.startswith("/"):
        raise ValueError(f"path must start with '/', got {path!r}")
    if urllib.parse.urlparse("http://x" + path).path != path:
        raise ValueError(
            f"path must not contain query parameters or fragments: {path!r}"
        )


@dataclass(frozen=True, slots=True)
class HealthClientConfig:
    """Configuration for the health client."""

    endpoint: HttpEndpointConfig
    liveness_path: str = "/healthz"
    readiness_path: str = "/readyz"

    def __post_init__(self) -> None:
        _validate_path(self.liveness_path)
        _validate_path(self.readiness_path)


@dataclass(frozen=True, slots=True)
class MetricsClientConfig:
    """Configuration for the metrics (OpenMetrics/Prometheus) client."""

    endpoint: HttpEndpointConfig
    metrics_path: str = "/metrics"

    def __post_init__(self) -> None:
        _validate_path(self.metrics_path)


@dataclass(frozen=True, slots=True)
class GatewayClientConfig:
    """Configuration for the application HTTP gateway client."""

    endpoint: HttpEndpointConfig


def _validate_port(port: int) -> None:
    if not isinstance(port, int) or not 1 <= port <= 65535:
        raise ValueError(f"port must be an integer in 1..65535, got {port}")


@dataclass(frozen=True, slots=True)
class CliClientConfig:
    """Configuration for the protobuf CLI client.

    Exactly one endpoint must be configured: either *uds_path* for a
    Unix-domain socket or *host* + *port* for TCP.
    """

    uds_path: str | None = "/tmp/hpactor/hpactor.sock"
    host: str | None = None
    port: int | None = None
    connect_timeout: float = 5.0
    request_timeout: float = 10.0
    max_outbound_payload: int = _MAX_RESPONSE_BYTES
    max_inbound_payload: int = _MAX_RESPONSE_BYTES
    default_format: str = "pretty"
    allow_insecure_remote_tcp: bool = False

    def __post_init__(self) -> None:
        uses_uds = self.uds_path is not None
        uses_tcp = self.host is not None and self.port is not None
        if uses_uds == uses_tcp:
            raise ConfigurationError(
                "CliClientConfig: set exactly one of uds_path or host+port"
            )
        if uses_tcp:
            _validate_port(self.port)  # type: ignore[arg-type]
            self._check_tcp_security()

    def _check_tcp_security(self) -> None:
        if self.allow_insecure_remote_tcp:
            return
        if self.host not in _LOOPBACK_HOSTS:  # type: ignore[operator]
            raise InsecureTransportError(
                f"CLI TCP to {self.host}:{self.port} requires "
                f"allow_insecure_remote_tcp=True"
            )


@dataclass(frozen=True, slots=True)
class HPActorClientConfig:
    """Aggregate configuration for the external client bundle.

    At least one capability must be configured.
    """

    health: HealthClientConfig | None = None
    metrics: MetricsClientConfig | None = None
    gateway: GatewayClientConfig | None = None
    cli: CliClientConfig | None = None

    def __post_init__(self) -> None:
        if not any((self.health, self.metrics, self.gateway, self.cli)):
            raise ValueError(
                "HPActorClientConfig: at least one capability must be configured"
            )
