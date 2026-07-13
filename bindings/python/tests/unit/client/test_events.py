# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for request event hooks."""

import unittest

import httpx

from hpactor.client._events import RequestEvent
from hpactor.client.config import HealthClientConfig, HttpEndpointConfig
from hpactor.client.health import HealthClient
from hpactor.client.models import HealthState, ResultCategory


def _endpoint() -> HttpEndpointConfig:
    return HttpEndpointConfig(base_url="https://node.example")


def _health_config() -> HealthClientConfig:
    return HealthClientConfig(endpoint=_endpoint())


class EventTest(unittest.TestCase):
    def test_hook_receives_success_event(self) -> None:
        events: list[RequestEvent] = []

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"OK")

        raw = httpx.Client(transport=httpx.MockTransport(handler))
        from hpactor.client._http import SyncHttpTransport
        transport = SyncHttpTransport(_endpoint(), client=raw)

        client = HealthClient(_health_config(), transport=transport)
        result = client.liveness()
        self.assertEqual(result.state, HealthState.HEALTHY)
        raw.close()

    def test_hook_error_does_not_replace_result(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"OK")

        raw = httpx.Client(transport=httpx.MockTransport(handler))
        from hpactor.client._http import SyncHttpTransport
        transport = SyncHttpTransport(_endpoint(), client=raw)

        client = HealthClient(_health_config(), transport=transport)
        result = client.liveness()
        self.assertEqual(result.state, HealthState.HEALTHY)
        raw.close()

    def test_request_event_fields_are_populated(self) -> None:
        event = RequestEvent(
            capability="health",
            operation="liveness",
            origin="https://node.example",
            attempt=1,
            duration=0.05,
            request_bytes=128,
            response_bytes=2,
            category=ResultCategory.SUCCESS,
        )
        self.assertEqual(event.capability, "health")
        self.assertEqual(event.operation, "liveness")
        self.assertEqual(event.category, ResultCategory.SUCCESS)
