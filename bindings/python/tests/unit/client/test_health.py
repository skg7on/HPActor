# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for sync/async health clients."""

import json
import unittest

import httpx

from hpactor.client.config import HealthClientConfig, HttpEndpointConfig
from hpactor.client.health import AsyncHealthClient, HealthClient
from hpactor.client.models import (
    HealthCheck,
    HealthProbe,
    HealthResult,
    HealthState,
)


def _endpoint() -> HttpEndpointConfig:
    return HttpEndpointConfig(base_url="https://node.example")


def _health_config(**overrides: object) -> HealthClientConfig:
    return HealthClientConfig(endpoint=_endpoint(), **overrides)


def _make_health_client(
    status: int,
    content_type: str | None,
    body: bytes,
) -> HealthClient:
    def handler(request: httpx.Request) -> httpx.Response:
        headers = {}
        if content_type:
            headers["content-type"] = content_type
        return httpx.Response(status, headers=headers, content=body)

    raw = httpx.Client(transport=httpx.MockTransport(handler))
    from hpactor.client._http import SyncHttpTransport
    transport = SyncHttpTransport(_endpoint(), client=raw)
    return HealthClient(_health_config(), transport=transport)


def _degraded_json() -> bytes:
    return json.dumps({
        "status": "degraded",
        "checks": [
            {"name": "scheduler", "status": "degraded", "reason": "backlog"},
        ],
    }).encode()


def _unhealthy_json() -> bytes:
    return json.dumps({
        "status": "unhealthy",
        "checks": [
            {"name": "network", "status": "unhealthy", "reason": "listener down"},
        ],
    }).encode()


class HealthClientTest(unittest.TestCase):
    def test_readiness_preserves_probe_and_body(self) -> None:
        client = _make_health_client(200, None, b"OK")
        result = client.readiness()
        self.assertEqual(result.probe, HealthProbe.READINESS)
        self.assertEqual(result.state, HealthState.HEALTHY)

    def test_liveness_default_path(self) -> None:
        client = _make_health_client(200, None, b"OK")
        result = client.liveness()
        self.assertEqual(result.probe, HealthProbe.LIVENESS)
        self.assertTrue(result.http_status, 200)

    def test_require_ready_raises_for_degraded(self) -> None:
        from hpactor.client.errors import HealthCheckFailed

        client = _make_health_client(200, "application/json", _degraded_json())
        with self.assertRaises(HealthCheckFailed) as caught:
            client.require_ready()
        self.assertEqual(caught.exception.result.state, HealthState.DEGRADED)

    def test_ok_body_is_healthy(self) -> None:
        result = _make_health_client(200, None, b"OK").liveness()
        self.assertEqual(result.state, HealthState.HEALTHY)

    def test_degraded_json_parsed(self) -> None:
        result = _make_health_client(200, "application/json", _degraded_json()).liveness()
        self.assertEqual(result.state, HealthState.DEGRADED)
        self.assertEqual(len(result.checks), 1)
        self.assertEqual(result.checks[0].name, "scheduler")

    def test_unhealthy_json_with_503(self) -> None:
        result = _make_health_client(503, "application/json", _unhealthy_json()).liveness()
        self.assertEqual(result.state, HealthState.UNHEALTHY)
        self.assertEqual(result.checks[0].name, "network")

    def test_require_live_raises_for_unhealthy(self) -> None:
        from hpactor.client.errors import HealthCheckFailed

        client = _make_health_client(503, "application/json", _unhealthy_json())
        with self.assertRaises(HealthCheckFailed):
            client.require_live()

    def test_non_2xx_non_503_raises_http_error(self) -> None:
        from hpactor.client.errors import HttpResponseError

        client = _make_health_client(500, "text/plain", b"internal error")
        with self.assertRaises(HttpResponseError):
            client.liveness()

    def test_non_json_content_type_with_ok_body(self) -> None:
        client = _make_health_client(200, "text/plain", b"OK")
        result = client.liveness()
        self.assertEqual(result.state, HealthState.HEALTHY)


class AsyncHealthClientTest(unittest.IsolatedAsyncioTestCase):
    async def test_async_readiness_ok(self) -> None:
        async def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"OK")

        raw = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        from hpactor.client._http import AsyncHttpTransport
        transport = AsyncHttpTransport(_endpoint(), client=raw)
        client = AsyncHealthClient(_health_config(), transport=transport)
        result = await client.readiness()
        self.assertEqual(result.state, HealthState.HEALTHY)
        await raw.aclose()

    async def test_async_degraded_json(self) -> None:
        async def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(
                200,
                headers={"content-type": "application/json"},
                content=_degraded_json(),
            )

        raw = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        from hpactor.client._http import AsyncHttpTransport
        transport = AsyncHttpTransport(_endpoint(), client=raw)
        client = AsyncHealthClient(_health_config(), transport=transport)
        result = await client.liveness()
        self.assertEqual(result.state, HealthState.DEGRADED)
        await raw.aclose()
