# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Bounded owned/injected sync and async HTTPX transport adapters."""

from __future__ import annotations

import math
import time
from typing import Optional

import httpx

from .config import HttpEndpointConfig, HttpLimits, RetryPolicy
from .errors import OperationTimeout, ResponseLimitError, TransportError


def _detached(response: httpx.Response, body: bytes) -> httpx.Response:
    """Return a fully buffered response with no open network stream."""
    return httpx.Response(
        response.status_code,
        headers=response.headers,
        content=body,
        request=response.request,
        extensions=dict(response.extensions),
    )


def _stream_and_bound(
    response: httpx.Response, limit: int
) -> bytes:
    """Read a streaming response body up to *limit* bytes.

    Raises ``ResponseLimitError`` if the body exceeds the limit.
    """
    body = b""
    for chunk in response.iter_bytes():
        if len(body) + len(chunk) > limit:
            raise ResponseLimitError(
                limit=limit,
                observed=len(body) + len(chunk),
                resource="HTTP response body",
            )
        body += chunk
    return body


def _should_retry(
    retry: RetryPolicy,
    attempt: int,
    response_or_err: httpx.Response | BaseException,
    idempotent: bool,
) -> bool:
    """Determine whether a request is eligible for another attempt."""
    if not idempotent:
        return False
    if attempt >= retry.attempts:
        return False
    if isinstance(response_or_err, BaseException):
        return isinstance(response_or_err, (
            httpx.ConnectError,
            httpx.ConnectTimeout,
            httpx.ReadTimeout,
            httpx.WriteTimeout,
            httpx.PoolTimeout,
            ConnectionError,
            TimeoutError,
        ))
    return response_or_err.status_code in retry.retry_statuses


def _sleep_or_raise(deadline: float, seconds: float) -> None:
    """Sleep for *seconds* without exceeding *deadline*."""
    now = time.monotonic()
    if now + seconds > deadline:
        raise OperationTimeout(phase="retry")
    if seconds > 0:
        time.sleep(min(seconds, deadline - now))


class SyncHttpTransport:
    """Bounded synchronous HTTP transport backed by an owned or injected
    ``httpx.Client``.
    """

    def __init__(
        self,
        endpoint: HttpEndpointConfig,
        *,
        client: httpx.Client | None = None,
    ) -> None:
        self._endpoint = endpoint
        self._owned: httpx.Client | None = None
        self._client: httpx.Client | None = client
        self._owns = client is None

    def _ensure(self) -> httpx.Client:
        if self._client is not None:
            return self._client
        c = httpx.Client(
            base_url=self._endpoint.base_url,
            headers=dict(self._endpoint.headers),
            verify=self._endpoint.verify,
            cert=self._endpoint.cert,
            follow_redirects=self._endpoint.follow_redirects,
            timeout=httpx.Timeout(
                connect=self._endpoint.timeouts.connect,
                read=self._endpoint.timeouts.read,
                write=self._endpoint.timeouts.write,
                pool=self._endpoint.timeouts.pool,
            ),
            limits=httpx.Limits(
                max_keepalive_connections=self._endpoint.limits.max_keepalive_connections,
                max_connections=self._endpoint.limits.max_connections,
            ),
        )
        self._owned = c
        self._client = c
        return c

    def request(
        self,
        method: str,
        url: str,
        *,
        idempotent: bool = False,
        **kwargs: object,
    ) -> httpx.Response:
        retry = self._endpoint.retry
        limit = self._endpoint.limits.max_response_bytes
        client = self._ensure()
        deadline = time.monotonic() + max(
            self._endpoint.timeouts.connect,
            self._endpoint.timeouts.read,
            self._endpoint.timeouts.write,
        )

        last_response: httpx.Response | None = None
        for attempt in range(1, retry.attempts + 1):
            try:
                response = client.request(method, url, **kwargs)
            except Exception as exc:
                if _should_retry(retry, attempt, exc, idempotent):
                    _sleep_or_raise(deadline, retry.initial_delay * (2 ** (attempt - 1)))
                    continue
                raise TransportError(str(exc)) from exc
            body = _stream_and_bound(response, limit)
            if _should_retry(retry, attempt, response, idempotent):
                _sleep_or_raise(deadline, retry.initial_delay * (2 ** (attempt - 1)))
                last_response = response
                continue
            return _detached(response, body)

        if last_response is not None:
            return _detached(last_response, _stream_and_bound(last_response, limit))
        raise TransportError("retry exhausted with no response")

    def close(self) -> None:
        if self._owns and self._owned is not None:
            self._owned.close()
            self._owned = None
            self._client = None


class AsyncHttpTransport:
    """Bounded asynchronous HTTP transport backed by an owned or injected
    ``httpx.AsyncClient``.
    """

    def __init__(
        self,
        endpoint: HttpEndpointConfig,
        *,
        client: httpx.AsyncClient | None = None,
    ) -> None:
        self._endpoint = endpoint
        self._owned: httpx.AsyncClient | None = None
        self._client: httpx.AsyncClient | None = client
        self._owns = client is None

    def _ensure(self) -> httpx.AsyncClient:
        if self._client is not None:
            return self._client
        c = httpx.AsyncClient(
            base_url=self._endpoint.base_url,
            headers=dict(self._endpoint.headers),
            verify=self._endpoint.verify,
            cert=self._endpoint.cert,
            follow_redirects=self._endpoint.follow_redirects,
            timeout=httpx.Timeout(
                connect=self._endpoint.timeouts.connect,
                read=self._endpoint.timeouts.read,
                write=self._endpoint.timeouts.write,
                pool=self._endpoint.timeouts.pool,
            ),
            limits=httpx.Limits(
                max_keepalive_connections=self._endpoint.limits.max_keepalive_connections,
                max_connections=self._endpoint.limits.max_connections,
            ),
        )
        self._owned = c
        self._client = c
        return c

    async def request(
        self,
        method: str,
        url: str,
        *,
        idempotent: bool = False,
        **kwargs: object,
    ) -> httpx.Response:
        retry = self._endpoint.retry
        limit = self._endpoint.limits.max_response_bytes
        client = self._ensure()
        deadline = time.monotonic() + max(
            self._endpoint.timeouts.connect,
            self._endpoint.timeouts.read,
            self._endpoint.timeouts.write,
        )

        last_response: httpx.Response | None = None
        for attempt in range(1, retry.attempts + 1):
            try:
                response = await client.request(method, url, **kwargs)
            except Exception as exc:
                if _should_retry(retry, attempt, exc, idempotent):
                    await self._asleep_or_raise(
                        deadline, retry.initial_delay * (2 ** (attempt - 1))
                    )
                    continue
                raise TransportError(str(exc)) from exc
            body = _stream_and_bound(response, limit)
            if _should_retry(retry, attempt, response, idempotent):
                await self._asleep_or_raise(
                    deadline, retry.initial_delay * (2 ** (attempt - 1))
                )
                last_response = response
                continue
            return _detached(response, body)

        if last_response is not None:
            return _detached(
                last_response,
                _stream_and_bound(last_response, limit),
            )
        raise TransportError("retry exhausted with no response")

    async def aclose(self) -> None:
        if self._owns and self._owned is not None:
            await self._owned.aclose()
            self._owned = None
            self._client = None

    @staticmethod
    async def _asleep_or_raise(deadline: float, seconds: float) -> None:
        import asyncio

        now = time.monotonic()
        if now + seconds > deadline:
            raise OperationTimeout(phase="retry")
        if seconds > 0:
            await asyncio.sleep(min(seconds, deadline - now))
