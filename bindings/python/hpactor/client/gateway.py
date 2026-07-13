# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Sync and async bounded HTTP gateway clients."""

from __future__ import annotations

from typing import Any

import httpx

from ._http import AsyncHttpTransport, SyncHttpTransport
from .config import GatewayClientConfig
from .errors import ConfigurationError, HttpResponseError


def _resolve_url(base_url: str, path: str, *, allow_absolute_url: bool = False) -> str:
    """Resolve a gateway path against the configured base URL.

    Absolute URLs are rejected unless *allow_absolute_url* is True.
    """
    if "://" in path:
        if not allow_absolute_url:
            raise ConfigurationError(
                f"Absolute URL {path!r} is not permitted without "
                f"allow_absolute_url=True"
            )
        return path
    if path.startswith("/"):
        return base_url.rstrip("/") + path
    return base_url.rstrip("/") + "/" + path


class GatewayClient:
    """Synchronous HTTP gateway client — preserves general HTTP semantics."""

    def __init__(
        self,
        config: GatewayClientConfig,
        *,
        transport: SyncHttpTransport | None = None,
    ) -> None:
        self._config = config
        self._transport = (
            transport if transport is not None else SyncHttpTransport(config.endpoint)
        )

    def request(
        self,
        method: str,
        path: str,
        *,
        idempotent: bool | None = None,
        allow_absolute_url: bool = False,
        **options: Any,
    ) -> httpx.Response:
        url = _resolve_url(
            self._config.endpoint.base_url,
            path,
            allow_absolute_url=allow_absolute_url,
        )
        safe = (
            method.upper() in {"GET", "HEAD"} if idempotent is None else idempotent
        )
        return self._transport.request(method, url, idempotent=safe, **options)

    def request_checked(
        self,
        method: str,
        path: str,
        *,
        idempotent: bool | None = None,
        allow_absolute_url: bool = False,
        **options: Any,
    ) -> httpx.Response:
        response = self.request(
            method,
            path,
            idempotent=idempotent,
            allow_absolute_url=allow_absolute_url,
            **options,
        )
        if not 200 <= response.status_code < 300:
            raise HttpResponseError.from_response(response)
        return response

    def get(self, path: str, **options: Any) -> httpx.Response:
        return self.request("GET", path, **options)

    def post(self, path: str, **options: Any) -> httpx.Response:
        return self.request("POST", path, **options)

    def put(self, path: str, **options: Any) -> httpx.Response:
        return self.request("PUT", path, **options)

    def patch(self, path: str, **options: Any) -> httpx.Response:
        return self.request("PATCH", path, **options)

    def delete(self, path: str, **options: Any) -> httpx.Response:
        return self.request("DELETE", path, **options)

    def close(self) -> None:
        self._transport.close()


class AsyncGatewayClient:
    """Asynchronous HTTP gateway client — preserves general HTTP semantics."""

    def __init__(
        self,
        config: GatewayClientConfig,
        *,
        transport: AsyncHttpTransport | None = None,
    ) -> None:
        self._config = config
        self._transport = (
            transport if transport is not None else AsyncHttpTransport(config.endpoint)
        )

    async def request(
        self,
        method: str,
        path: str,
        *,
        idempotent: bool | None = None,
        allow_absolute_url: bool = False,
        **options: Any,
    ) -> httpx.Response:
        url = _resolve_url(
            self._config.endpoint.base_url,
            path,
            allow_absolute_url=allow_absolute_url,
        )
        safe = (
            method.upper() in {"GET", "HEAD"} if idempotent is None else idempotent
        )
        return await self._transport.request(method, url, idempotent=safe, **options)

    async def request_checked(
        self,
        method: str,
        path: str,
        *,
        idempotent: bool | None = None,
        allow_absolute_url: bool = False,
        **options: Any,
    ) -> httpx.Response:
        response = await self.request(
            method,
            path,
            idempotent=idempotent,
            allow_absolute_url=allow_absolute_url,
            **options,
        )
        if not 200 <= response.status_code < 300:
            raise HttpResponseError.from_response(response)
        return response

    async def get(self, path: str, **options: Any) -> httpx.Response:
        return await self.request("GET", path, **options)

    async def post(self, path: str, **options: Any) -> httpx.Response:
        return await self.request("POST", path, **options)

    async def put(self, path: str, **options: Any) -> httpx.Response:
        return await self.request("PUT", path, **options)

    async def patch(self, path: str, **options: Any) -> httpx.Response:
        return await self.request("PATCH", path, **options)

    async def delete(self, path: str, **options: Any) -> httpx.Response:
        return await self.request("DELETE", path, **options)

    async def aclose(self) -> None:
        await self._transport.aclose()
