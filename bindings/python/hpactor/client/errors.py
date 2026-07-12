# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Typed exceptions for the HPActor external client SDK."""

from __future__ import annotations

from typing import Any


class HPActorClientError(Exception):
    """Base exception for all external SDK failures."""


class ConfigurationError(HPActorClientError):
    """A configuration value is invalid or inconsistent."""


class InsecureTransportError(ConfigurationError):
    """An insecure transport was requested without explicit opt-in."""


class UnsupportedCapability(HPActorClientError):
    """Accessing a bundle capability that was not configured."""

    def __init__(self, capability: str):
        super().__init__(
            f"Capability {capability!r} is not configured on this client"
        )
        self.capability = capability


class NativeBindingUnavailable(HPActorClientError):
    """Importing hpactor._hpactor failed in this environment."""


class ClientClosedError(HPActorClientError):
    """Operation attempted after the client or bundle was closed."""


class EventLoopMismatchError(HPActorClientError):
    """Async client used from a different event loop than the binding loop."""


class TransportError(HPActorClientError):
    """A connection, read, or write transport error."""


class ConnectionError(TransportError):
    """Failed to establish a connection."""


class OperationTimeout(TransportError):
    """An operation exceeded its deadline."""

    def __init__(self, phase: str = "total"):
        super().__init__(f"Operation timed out during phase: {phase}")
        self.phase = phase


class ResponseLimitError(HPActorClientError):
    """A response body, frame, or payload exceeded its configured bound."""

    def __init__(self, *, limit: int, observed: int = 0, resource: str = ""):
        msg = f"{resource} exceeded limit of {limit} bytes"
        if observed:
            msg += f" (observed {observed} bytes)"
        super().__init__(msg)
        self.limit = limit
        self.observed = observed
        self.resource = resource


class ProtocolError(HPActorClientError):
    """The server replied with a malformed or unexpected protocol payload."""


def _make_http_response_error(
    status_code: int, *, body: bytes | None = None
) -> "HttpResponseError":
    detail = b""
    if body:
        detail = body[:4096]
    return HttpResponseError(status_code, detail=detail)


class HttpResponseError(HPActorClientError):
    """An HTTP response with a non-success status code."""

    def __init__(self, status_code: int, *, detail: bytes = b""):
        super().__init__(f"HTTP {status_code}")
        self.status_code = status_code
        self.detail = detail

    @classmethod
    def from_response(cls, response: Any) -> "HttpResponseError":
        return _make_http_response_error(
            response.status_code, body=response.content
        )


class HealthCheckFailed(HPActorClientError):
    """A health check requirement (liveness or readiness) was not met."""

    def __init__(self, message: str = "", *, result: Any = None):
        super().__init__(message or "Health check failed")
        self.result = result


class CliCommandError(HPActorClientError):
    """The CLI server returned an error for a command."""

    def __init__(
        self,
        error_code: int = 0,
        *,
        payload: bytes = b"",
        content_type: str = "",
    ):
        detail = payload[:4096]
        msg = f"CLI command failed with error code {error_code}"
        if detail:
            msg += f": {detail.decode('utf-8', errors='replace')}"
        super().__init__(msg)
        self.error_code = error_code
        self.payload = detail
        self.content_type = content_type
