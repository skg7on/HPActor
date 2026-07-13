# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for sync/async HTTP gateway clients."""

import unittest

import httpx

from hpactor.client.config import GatewayClientConfig, HttpEndpointConfig
from hpactor.client.errors import ConfigurationError, HttpResponseError
from hpactor.client.gateway import AsyncGatewayClient, GatewayClient


def _endpoint() -> HttpEndpointConfig:
    return HttpEndpointConfig(base_url="https://node.example")


def _gateway_config() -> GatewayClientConfig:
    return GatewayClientConfig(endpoint=_endpoint())


def _make_gateway_client(
    status: int,
    body: bytes = b"",
    *,
    headers: dict | None = None,
) -> tuple[GatewayClient, list[httpx.Request]]:
    seen: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        return httpx.Response(status, headers=headers or {}, content=body)

    raw = httpx.Client(transport=httpx.MockTransport(handler))
    from hpactor.client._http import SyncHttpTransport
    transport = SyncHttpTransport(_endpoint(), client=raw)
    return GatewayClient(_gateway_config(), transport=transport), seen


class GatewayClientTest(unittest.TestCase):
    def test_non_success_is_returned_unchanged(self) -> None:
        client, seen = _make_gateway_client(429, b"busy")
        response = client.post("/orders", json={"id": 7})
        self.assertEqual(response.status_code, 429)
        self.assertEqual(response.content, b"busy")
        self.assertEqual(seen[0].method, "POST")

    def test_absolute_url_is_rejected_by_default(self) -> None:
        client, _ = _make_gateway_client(200)
        with self.assertRaises(ConfigurationError):
            client.get("https://other.example/escape")

    def test_checked_request_maps_non_success(self) -> None:
        client, _ = _make_gateway_client(404, b"missing")
        with self.assertRaises(HttpResponseError) as caught:
            client.request_checked("GET", "/unknown")
        self.assertEqual(caught.exception.status_code, 404)

    def test_checked_request_returns_2xx(self) -> None:
        client, _ = _make_gateway_client(200, b"ok")
        response = client.request_checked("GET", "/known")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.content, b"ok")

    def test_convenience_methods_delegate_to_request(self) -> None:
        client, seen = _make_gateway_client(200, b"ok")
        client.get("/items")
        self.assertEqual(seen[0].method, "GET")
        client.put("/items/1", content=b"data")
        self.assertEqual(seen[-1].method, "PUT")
        client.delete("/items/1")
        self.assertEqual(seen[-1].method, "DELETE")

    def test_path_with_query_is_preserved(self) -> None:
        client, seen = _make_gateway_client(200)
        client.get("/items?page=2")
        self.assertIn("?page=2", str(seen[0].url))
