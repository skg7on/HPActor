# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for sync/async metrics clients."""

import unittest

import httpx

from hpactor.client.config import HttpEndpointConfig, MetricsClientConfig
from hpactor.client.errors import HttpResponseError, ProtocolError
from hpactor.client.metrics import AsyncMetricsClient, MetricsClient
from hpactor.client.models import MetricsNotModified, MetricsSnapshot


def _endpoint() -> HttpEndpointConfig:
    return HttpEndpointConfig(base_url="https://node.example")


def _metrics_config() -> MetricsClientConfig:
    return MetricsClientConfig(endpoint=_endpoint())


def _make_metrics_client(
    status: int,
    content_type: str | None,
    body: bytes,
    *,
    etag: str | None = None,
) -> MetricsClient:
    def handler(request: httpx.Request) -> httpx.Response:
        headers = {}
        if content_type:
            headers["content-type"] = content_type
        if etag:
            headers["etag"] = etag
        return httpx.Response(status, headers=headers, content=body)

    raw = httpx.Client(transport=httpx.MockTransport(handler))
    from hpactor.client._http import SyncHttpTransport
    transport = SyncHttpTransport(_endpoint(), client=raw)
    return MetricsClient(_metrics_config(), transport=transport)


class MetricsClientTest(unittest.TestCase):
    def test_scrape_preserves_exposition_text(self) -> None:
        body = b"# TYPE hpactor_actor_count gauge\nhpactor_actor_count 3\n"
        client = _make_metrics_client(
            200, "text/plain; version=0.0.4", body, etag='"snap-7"'
        )
        result = client.scrape()
        self.assertIsInstance(result, MetricsSnapshot)
        self.assertEqual(result.text.encode(), body)
        self.assertEqual(result.etag, '"snap-7"')

    def test_etag_304_is_not_an_empty_snapshot(self) -> None:
        client = _make_metrics_client(304, None, b"")
        result = client.scrape(etag='"snap-7"')
        self.assertIsInstance(result, MetricsNotModified)
        self.assertEqual(result.etag, '"snap-7"')

    def test_non_2xx_raises_http_error(self) -> None:
        client = _make_metrics_client(500, "text/plain", b"error")
        with self.assertRaises(HttpResponseError):
            client.scrape()

    def test_invalid_utf8_raises_protocol_error(self) -> None:
        client = _make_metrics_client(
            200, "text/plain; version=1.0.0", b"\xff\xfe"
        )
        with self.assertRaises(ProtocolError):
            client.scrape()

    def test_openmetrics_content_type_accepted(self) -> None:
        body = b"# EOF\n"
        client = _make_metrics_client(
            200, "application/openmetrics-text; version=1.0.0", body
        )
        result = client.scrape()
        self.assertIsInstance(result, MetricsSnapshot)
        self.assertEqual(result.text, "# EOF\n")


class AsyncMetricsClientTest(unittest.IsolatedAsyncioTestCase):
    async def test_async_scrape_preserves_text(self) -> None:
        async def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(
                200,
                headers={"content-type": "text/plain; version=0.0.4"},
                content=b"hpactor_actor_count 5\n",
            )

        raw = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        from hpactor.client._http import AsyncHttpTransport
        transport = AsyncHttpTransport(_endpoint(), client=raw)
        client = AsyncMetricsClient(_metrics_config(), transport=transport)
        result = await client.scrape()
        self.assertIsInstance(result, MetricsSnapshot)
        self.assertIn("hpactor_actor_count", result.text)
        await raw.aclose()
