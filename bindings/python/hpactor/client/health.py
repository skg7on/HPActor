# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Sync and async health clients with conservative response parsing."""

from __future__ import annotations

import json
import time
from typing import Optional

import httpx

from ._http import AsyncHttpTransport, SyncHttpTransport
from .config import HealthClientConfig
from .errors import HealthCheckFailed, HttpResponseError, ProtocolError
from .models import HealthCheck, HealthProbe, HealthResult, HealthState


def _is_json(response: httpx.Response) -> bool:
    ct = response.headers.get("content-type", "")
    return "application/json" in ct


def _parse_health_json(body: bytes) -> dict:
    try:
        payload = json.loads(body)
    except json.JSONDecodeError as exc:
        raise ProtocolError("health JSON is malformed") from exc
    if not isinstance(payload, dict):
        raise ProtocolError("health JSON must be an object")
    if "status" not in payload or not isinstance(payload["status"], str):
        raise ProtocolError("health JSON missing 'status' field")
    return payload


def _parse_checks(payload: dict) -> tuple[HealthCheck, ...]:
    raw_checks = payload.get("checks")
    if raw_checks is None:
        return ()
    if not isinstance(raw_checks, list):
        raise ProtocolError("health 'checks' must be an array")
    checks: list[HealthCheck] = []
    for item in raw_checks:
        if not isinstance(item, dict):
            raise ProtocolError("each health check must be an object")
        name = item.get("name")
        if not isinstance(name, str):
            raise ProtocolError("health check 'name' must be a string")
        status_str = item.get("status", "unknown")
        if not isinstance(status_str, str):
            raise ProtocolError("health check 'status' must be a string")
        reason = item.get("reason", "")
        if not isinstance(reason, str):
            reason = str(reason)
        checks.append(HealthCheck(name, HealthState.from_wire(status_str), reason))
    return tuple(checks)


def _parse_health(
    probe: HealthProbe,
    response: httpx.Response,
    elapsed: float,
) -> HealthResult:
    body = response.content
    content_type = response.headers.get("content-type", "")

    # "OK" body with 2xx → HEALTHY (current HealthHttpServer behavior)
    if 200 <= response.status_code < 300 and body.strip() == b"OK":
        return HealthResult(
            probe=probe,
            state=HealthState.HEALTHY,
            http_status=response.status_code,
            checks=(),
            content_type=content_type,
            raw_body=body,
            elapsed=elapsed,
        )

    # JSON health response
    if _is_json(response):
        payload = _parse_health_json(body)
        state = HealthState.from_wire(payload["status"])
        checks = _parse_checks(payload)
        if response.status_code == 503 and state is HealthState.UNHEALTHY:
            return HealthResult(
                probe=probe,
                state=state,
                http_status=503,
                checks=checks,
                content_type=content_type,
                raw_body=body,
                elapsed=elapsed,
            )
        if 200 <= response.status_code < 300:
            return HealthResult(
                probe=probe,
                state=state,
                http_status=response.status_code,
                checks=checks,
                content_type=content_type,
                raw_body=body,
                elapsed=elapsed,
            )

    raise HttpResponseError.from_response(response)


class HealthClient:
    """Synchronous health client."""

    def __init__(
        self,
        config: HealthClientConfig,
        *,
        transport: SyncHttpTransport | None = None,
    ) -> None:
        self._config = config
        self._transport = transport if transport is not None else SyncHttpTransport(config.endpoint)

    def liveness(self) -> HealthResult:
        start = time.monotonic()
        url = self._config.endpoint.base_url + self._config.liveness_path
        response = self._transport.request("GET", url)
        elapsed = time.monotonic() - start
        return _parse_health(HealthProbe.LIVENESS, response, elapsed)

    def readiness(self) -> HealthResult:
        start = time.monotonic()
        url = self._config.endpoint.base_url + self._config.readiness_path
        response = self._transport.request("GET", url)
        elapsed = time.monotonic() - start
        return _parse_health(HealthProbe.READINESS, response, elapsed)

    def require_live(self) -> HealthResult:
        result = self.liveness()
        if result.state is not HealthState.HEALTHY:
            raise HealthCheckFailed(
                f"Health check failed: {result.state.value}", result=result
            )
        return result

    def require_ready(self) -> HealthResult:
        result = self.readiness()
        if result.state is not HealthState.HEALTHY:
            raise HealthCheckFailed(
                f"Health check failed: {result.state.value}", result=result
            )
        return result

    def close(self) -> None:
        self._transport.close()


class AsyncHealthClient:
    """Asynchronous health client."""

    def __init__(
        self,
        config: HealthClientConfig,
        *,
        transport: AsyncHttpTransport | None = None,
    ) -> None:
        self._config = config
        self._transport = (
            transport if transport is not None else AsyncHttpTransport(config.endpoint)
        )

    async def liveness(self) -> HealthResult:
        start = time.monotonic()
        url = self._config.endpoint.base_url + self._config.liveness_path
        response = await self._transport.request("GET", url)
        elapsed = time.monotonic() - start
        return _parse_health(HealthProbe.LIVENESS, response, elapsed)

    async def readiness(self) -> HealthResult:
        start = time.monotonic()
        url = self._config.endpoint.base_url + self._config.readiness_path
        response = await self._transport.request("GET", url)
        elapsed = time.monotonic() - start
        return _parse_health(HealthProbe.READINESS, response, elapsed)

    async def require_live(self) -> HealthResult:
        result = await self.liveness()
        if result.state is not HealthState.HEALTHY:
            raise HealthCheckFailed(
                f"Health check failed: {result.state.value}", result=result
            )
        return result

    async def require_ready(self) -> HealthResult:
        result = await self.readiness()
        if result.state is not HealthState.HEALTHY:
            raise HealthCheckFailed(
                f"Health check failed: {result.state.value}", result=result
            )
        return result

    async def aclose(self) -> None:
        await self._transport.aclose()
