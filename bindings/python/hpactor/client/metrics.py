# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Sync and async raw metrics (OpenMetrics/Prometheus) retrieval."""

from __future__ import annotations

import time
from datetime import datetime, timezone
from typing import Optional

import httpx

from ._http import AsyncHttpTransport, SyncHttpTransport
from .config import MetricsClientConfig
from .errors import HttpResponseError, ProtocolError
from .models import MetricsNotModified, MetricsSnapshot

_ACCEPTED_MEDIA_TYPES = frozenset({
    "text/plain",
    "application/openmetrics-text",
})


def _extract_media_type(content_type: str | None) -> str:
    if not content_type:
        return ""
    base = content_type.split(";")[0].strip().lower()
    return base


def _parse_metrics(
    response: httpx.Response,
    collected_at: datetime,
    elapsed: float,
    requested_etag: str | None,
) -> MetricsSnapshot | MetricsNotModified:
    if response.status_code == 304 and requested_etag is not None:
        return MetricsNotModified(requested_etag)

    if not 200 <= response.status_code < 300:
        raise HttpResponseError.from_response(response)

    content_type = response.headers.get("content-type", "")
    media_type = _extract_media_type(content_type)

    try:
        text = response.content.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise ProtocolError("metrics body is not UTF-8") from exc

    return MetricsSnapshot(
        text=text,
        content_type=media_type,
        http_status=response.status_code,
        collected_at=collected_at,
        elapsed=elapsed,
        etag=response.headers.get("etag"),
    )


class MetricsClient:
    """Synchronous raw metrics client."""

    def __init__(
        self,
        config: MetricsClientConfig,
        *,
        transport: SyncHttpTransport | None = None,
    ) -> None:
        self._config = config
        self._transport = (
            transport if transport is not None else SyncHttpTransport(config.endpoint)
        )

    def scrape(self, *, etag: str | None = None) -> MetricsSnapshot | MetricsNotModified:
        start = time.monotonic()
        url = self._config.endpoint.base_url + self._config.metrics_path
        headers: dict[str, str] = {
            "Accept": "text/plain; version=1.0.0, application/openmetrics-text; version=1.0.0",
        }
        if etag:
            headers["If-None-Match"] = etag
        response = self._transport.request("GET", url, headers=headers)
        elapsed = time.monotonic() - start
        return _parse_metrics(
            response,
            datetime.now(timezone.utc),
            elapsed,
            etag,
        )

    def close(self) -> None:
        self._transport.close()


class AsyncMetricsClient:
    """Asynchronous raw metrics client."""

    def __init__(
        self,
        config: MetricsClientConfig,
        *,
        transport: AsyncHttpTransport | None = None,
    ) -> None:
        self._config = config
        self._transport = (
            transport if transport is not None else AsyncHttpTransport(config.endpoint)
        )

    async def scrape(
        self, *, etag: str | None = None
    ) -> MetricsSnapshot | MetricsNotModified:
        start = time.monotonic()
        url = self._config.endpoint.base_url + self._config.metrics_path
        headers: dict[str, str] = {
            "Accept": "text/plain; version=1.0.0, application/openmetrics-text; version=1.0.0",
        }
        if etag:
            headers["If-None-Match"] = etag
        response = await self._transport.request("GET", url, headers=headers)
        elapsed = time.monotonic() - start
        return _parse_metrics(
            response,
            datetime.now(timezone.utc),
            elapsed,
            etag,
        )

    async def aclose(self) -> None:
        await self._transport.aclose()
