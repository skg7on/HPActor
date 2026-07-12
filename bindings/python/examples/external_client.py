#!/usr/bin/env python3
# Copyright 2026 HPActor Contributors (Apache 2.0)
"""External SDK example: sync and async health, metrics, gateway, and CLI usage.

Usage:
    python3 external_client.py --base-url http://127.0.0.1:8080
    python3 external_client.py --base-url http://127.0.0.1:8080 --async
    python3 external_client.py --base-url http://127.0.0.1:8080 --cli-socket /tmp/hpactor/hpactor.sock
"""

import argparse
import asyncio
import sys

from hpactor.client import (
    AsyncHPActorClient,
    CliClientConfig,
    GatewayClientConfig,
    HealthClientConfig,
    HPActorClient,
    HPActorClientConfig,
    HttpEndpointConfig,
    MetricsClientConfig,
)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="HPActor external SDK example"
    )
    parser.add_argument("--base-url", required=True, help="Base URL for HTTP endpoints")
    parser.add_argument("--cli-socket", default=None, help="CLI UDS path")
    parser.add_argument("--async", dest="use_async", action="store_true", help="Use async clients")
    return parser.parse_args()


def _build_config(args: argparse.Namespace) -> HPActorClientConfig:
    endpoint = HttpEndpointConfig(base_url=args.base_url)
    return HPActorClientConfig(
        health=HealthClientConfig(endpoint=endpoint),
        metrics=MetricsClientConfig(endpoint=endpoint),
        gateway=GatewayClientConfig(endpoint=endpoint),
        cli=CliClientConfig(uds_path=args.cli_socket) if args.cli_socket else None,
    )


def _run_sync(config: HPActorClientConfig) -> None:
    with HPActorClient(config) as client:
        readiness = client.health.readiness()
        print(f"Health: {readiness.state.value} (probe={readiness.probe.value})")

        snapshot = client.metrics.scrape()
        if hasattr(snapshot, "text"):
            print(f"Metrics: {len(snapshot.text)} bytes")  # type: ignore[union-attr]

        gateway_resp = client.gateway.get("/echo")
        print(f"Gateway GET /echo: {gateway_resp.status_code}")

        if client.capabilities and hasattr(client, "cli") and client.cli is not None:
            stats = client.cli.system_stats()
            print(f"Workers: {stats.worker_count}")


async def _run_async(config: HPActorClientConfig) -> None:
    async with AsyncHPActorClient(config) as client:
        readiness = await client.health.readiness()
        print(f"Health: {readiness.state.value} (probe={readiness.probe.value})")

        snapshot = await client.metrics.scrape()
        if hasattr(snapshot, "text"):
            print(f"Metrics: {len(snapshot.text)} bytes")  # type: ignore[union-attr]

        print("All checks complete.")


def main() -> None:
    args = _parse_args()
    config = _build_config(args)

    if args.use_async:
        asyncio.run(_run_async(config))
    else:
        _run_sync(config)


if __name__ == "__main__":
    main()
