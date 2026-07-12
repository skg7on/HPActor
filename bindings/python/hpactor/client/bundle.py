# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Configured capability bundles with deterministic close."""

from __future__ import annotations

from typing import Optional

from ._http import AsyncHttpTransport, SyncHttpTransport
from .cli import AsyncCliClient, CliClient
from .config import HPActorClientConfig
from .errors import UnsupportedCapability
from .gateway import AsyncGatewayClient, GatewayClient
from .health import AsyncHealthClient, HealthClient
from .metrics import AsyncMetricsClient, MetricsClient
from .models import Capability


def _capabilities(config: HPActorClientConfig) -> frozenset[Capability]:
    caps: set[Capability] = set()
    if config.health is not None:
        caps.add(Capability.HEALTH)
    if config.metrics is not None:
        caps.add(Capability.METRICS)
    if config.gateway is not None:
        caps.add(Capability.GATEWAY)
    if config.cli is not None:
        caps.add(Capability.CLI)
    return frozenset(caps)


class HPActorClient:
    """Synchronous client bundle — exposes only configured capabilities."""

    def __init__(self, config: HPActorClientConfig) -> None:
        self._config = config
        self._caps = _capabilities(config)
        self._health: Optional[HealthClient] = None
        self._metrics: Optional[MetricsClient] = None
        self._gateway: Optional[GatewayClient] = None
        self._cli: Optional[CliClient] = None
        self._closed = False

    @property
    def capabilities(self) -> frozenset[Capability]:
        return self._caps

    @property
    def health(self) -> HealthClient:
        if Capability.HEALTH not in self._caps:
            raise UnsupportedCapability("health")
        if self._health is None:
            self._health = HealthClient(self._config.health)  # type: ignore[arg-type]
        return self._health

    @property
    def metrics(self) -> MetricsClient:
        if Capability.METRICS not in self._caps:
            raise UnsupportedCapability("metrics")
        if self._metrics is None:
            self._metrics = MetricsClient(self._config.metrics)  # type: ignore[arg-type]
        return self._metrics

    @property
    def gateway(self) -> GatewayClient:
        if Capability.GATEWAY not in self._caps:
            raise UnsupportedCapability("gateway")
        if self._gateway is None:
            self._gateway = GatewayClient(self._config.gateway)  # type: ignore[arg-type]
        return self._gateway

    @property
    def cli(self) -> CliClient:
        if Capability.CLI not in self._caps:
            raise UnsupportedCapability("cli")
        if self._cli is None:
            self._cli = CliClient(self._config.cli)  # type: ignore[arg-type]
        return self._cli

    def close(self) -> None:
        errors: list[Exception] = []
        for client in (self._cli, self._gateway, self._metrics, self._health):
            if client is not None:
                try:
                    client.close()
                except Exception as exc:
                    errors.append(exc)
        if errors:
            first = errors[0]
            for other in errors[1:]:
                first.add_note(f"close error: {other}")
            raise first

    def __enter__(self) -> "HPActorClient":
        return self

    def __exit__(self, *args: object) -> None:
        self.close()


class AsyncHPActorClient:
    """Asynchronous client bundle — exposes only configured capabilities."""

    def __init__(self, config: HPActorClientConfig) -> None:
        self._config = config
        self._caps = _capabilities(config)
        self._health: Optional[AsyncHealthClient] = None
        self._metrics: Optional[AsyncMetricsClient] = None
        self._gateway: Optional[AsyncGatewayClient] = None
        self._cli: Optional[AsyncCliClient] = None
        self._closed = False

    @property
    def capabilities(self) -> frozenset[Capability]:
        return self._caps

    @property
    def health(self) -> AsyncHealthClient:
        if Capability.HEALTH not in self._caps:
            raise UnsupportedCapability("health")
        if self._health is None:
            self._health = AsyncHealthClient(self._config.health)  # type: ignore[arg-type]
        return self._health

    @property
    def metrics(self) -> AsyncMetricsClient:
        if Capability.METRICS not in self._caps:
            raise UnsupportedCapability("metrics")
        if self._metrics is None:
            self._metrics = AsyncMetricsClient(self._config.metrics)  # type: ignore[arg-type]
        return self._metrics

    @property
    def gateway(self) -> AsyncGatewayClient:
        if Capability.GATEWAY not in self._caps:
            raise UnsupportedCapability("gateway")
        if self._gateway is None:
            self._gateway = AsyncGatewayClient(self._config.gateway)  # type: ignore[arg-type]
        return self._gateway

    @property
    def cli(self) -> AsyncCliClient:
        if Capability.CLI not in self._caps:
            raise UnsupportedCapability("cli")
        if self._cli is None:
            self._cli = AsyncCliClient(self._config.cli)  # type: ignore[arg-type]
        return self._cli

    async def aclose(self) -> None:
        errors: list[Exception] = []
        for client in (self._cli, self._gateway, self._metrics, self._health):
            if client is not None:
                try:
                    await client.aclose()
                except Exception as exc:
                    errors.append(exc)
        if errors:
            first = errors[0]
            for other in errors[1:]:
                first.add_note(f"close error: {other}")
            raise first

    async def __aenter__(self) -> "AsyncHPActorClient":
        return self

    async def __aexit__(self, *args: object) -> None:
        await self.aclose()
