# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for bounded sync/async HTTP transport adapters."""

import unittest

import httpx

from hpactor.client._http import AsyncHttpTransport, SyncHttpTransport
from hpactor.client.config import HttpEndpointConfig, HttpLimits, RetryPolicy
from hpactor.client.errors import OperationTimeout, ResponseLimitError


def _endpoint(**overrides: object) -> HttpEndpointConfig:
    kwargs = {"base_url": "https://node.example"}
    kwargs.update(overrides)  # type: ignore[arg-type]
    return HttpEndpointConfig(**kwargs)  # type: ignore[arg-type]


class SyncHttpTransportTest(unittest.TestCase):
    def test_owned_transport_close_is_idempotent(self) -> None:
        transport = SyncHttpTransport(_endpoint())
        transport.close()
        transport.close()  # should not raise

    def test_injected_transport_never_closes_raw(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"ok")

        raw = httpx.Client(transport=httpx.MockTransport(handler))
        transport = SyncHttpTransport(_endpoint(), client=raw)
        self.assertFalse(raw.is_closed)
        transport.close()
        self.assertFalse(raw.is_closed)
        raw.close()

    def test_stream_stops_before_exceeding_bound(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"x" * 17)

        endpoint = _endpoint(
            limits=HttpLimits(max_response_bytes=16),
        )
        raw = httpx.Client(transport=httpx.MockTransport(handler))
        transport = SyncHttpTransport(endpoint, client=raw)
        with self.assertRaises(ResponseLimitError) as caught:
            transport.request("GET", endpoint.base_url + "/metrics")
        self.assertEqual(caught.exception.limit, 16)
        self.assertFalse(raw.is_closed)
        raw.close()

    def test_response_body_at_limit_is_returned(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"a" * 16)

        endpoint = _endpoint(
            limits=HttpLimits(max_response_bytes=16),
        )
        raw = httpx.Client(transport=httpx.MockTransport(handler))
        transport = SyncHttpTransport(endpoint, client=raw)
        response = transport.request("GET", endpoint.base_url + "/metrics")
        self.assertEqual(response.content, b"a" * 16)
        raw.close()

    def test_retry_is_bounded_by_attempt_count(self) -> None:
        calls = []

        def handler(request: httpx.Request) -> httpx.Response:
            calls.append(1)
            return httpx.Response(503)

        endpoint = _endpoint(
            retry=RetryPolicy(attempts=3),
        )
        raw = httpx.Client(transport=httpx.MockTransport(handler))
        transport = SyncHttpTransport(endpoint, client=raw)
        response = transport.request(
            "GET", endpoint.base_url + "/readyz", idempotent=True
        )
        self.assertEqual(response.status_code, 503)
        self.assertEqual(len(calls), 3)
        raw.close()

    def test_non_idempotent_requests_not_retried(self) -> None:
        calls = []

        def handler(request: httpx.Request) -> httpx.Response:
            calls.append(1)
            return httpx.Response(503)

        endpoint = _endpoint(
            retry=RetryPolicy(attempts=3),
        )
        raw = httpx.Client(transport=httpx.MockTransport(handler))
        transport = SyncHttpTransport(endpoint, client=raw)
        response = transport.request("POST", endpoint.base_url + "/data")
        self.assertEqual(response.status_code, 503)
        self.assertEqual(len(calls), 1)
        raw.close()

    def test_non_2xx_is_returned_unchanged(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(404, content=b"not found")

        raw = httpx.Client(transport=httpx.MockTransport(handler))
        endpoint = _endpoint()
        transport = SyncHttpTransport(endpoint, client=raw)
        response = transport.request("GET", endpoint.base_url + "/missing")
        self.assertEqual(response.status_code, 404)
        self.assertEqual(response.content, b"not found")
        raw.close()


class AsyncHttpTransportTest(unittest.IsolatedAsyncioTestCase):
    async def test_owned_transport_aclose_is_idempotent(self) -> None:
        transport = AsyncHttpTransport(_endpoint())
        await transport.aclose()
        await transport.aclose()  # should not raise

    async def test_injected_transport_never_closes_raw(self) -> None:
        async def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"ok")

        raw = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        transport = AsyncHttpTransport(_endpoint(), client=raw)
        self.assertFalse(raw.is_closed)
        await transport.aclose()
        self.assertFalse(raw.is_closed)
        await raw.aclose()

    async def test_async_stream_stops_before_exceeding_bound(self) -> None:
        async def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=b"x" * 17)

        endpoint = _endpoint(
            limits=HttpLimits(max_response_bytes=16),
        )
        raw = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        transport = AsyncHttpTransport(endpoint, client=raw)
        with self.assertRaises(ResponseLimitError) as caught:
            await transport.request("GET", endpoint.base_url + "/metrics")
        self.assertEqual(caught.exception.limit, 16)
        self.assertFalse(raw.is_closed)
        await raw.aclose()

    async def test_async_retry_is_bounded_by_attempt_count(self) -> None:
        calls = []

        async def handler(request: httpx.Request) -> httpx.Response:
            calls.append(1)
            return httpx.Response(503)

        endpoint = _endpoint(
            retry=RetryPolicy(attempts=3),
        )
        raw = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        transport = AsyncHttpTransport(endpoint, client=raw)
        response = await transport.request(
            "GET", endpoint.base_url + "/readyz", idempotent=True
        )
        self.assertEqual(response.status_code, 503)
        self.assertEqual(len(calls), 3)
        await raw.aclose()

    async def test_async_non_idempotent_requests_not_retried(self) -> None:
        calls = []

        async def handler(request: httpx.Request) -> httpx.Response:
            calls.append(1)
            return httpx.Response(503)

        endpoint = _endpoint(
            retry=RetryPolicy(attempts=3),
        )
        raw = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        transport = AsyncHttpTransport(endpoint, client=raw)
        response = await transport.request(
            "POST", endpoint.base_url + "/data"
        )
        self.assertEqual(response.status_code, 503)
        self.assertEqual(len(calls), 1)
        await raw.aclose()
