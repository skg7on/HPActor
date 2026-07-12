# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for client capability bundles."""

import unittest

import httpx

from hpactor.client.bundle import HPActorClient
from hpactor.client.config import (
    CliClientConfig,
    HealthClientConfig,
    HPActorClientConfig,
    HttpEndpointConfig,
)
from hpactor.client.errors import UnsupportedCapability
from hpactor.client.models import Capability


def _endpoint() -> HttpEndpointConfig:
    return HttpEndpointConfig(base_url="https://node.example")


def _health_config() -> HealthClientConfig:
    return HealthClientConfig(endpoint=_endpoint())


class BundleTest(unittest.TestCase):
    def test_missing_capability_is_typed(self) -> None:
        config = HPActorClientConfig(health=_health_config())
        client = HPActorClient(config)
        self.assertEqual(client.capabilities, frozenset({Capability.HEALTH}))
        with self.assertRaises(UnsupportedCapability) as caught:
            _ = client.cli
        self.assertEqual(caught.exception.capability, "cli")

    def test_bundle_close_is_idempotent(self) -> None:
        config = HPActorClientConfig(health=_health_config())
        client = HPActorClient(config)
        client.close()
        client.close()  # Should not raise

    def test_configured_capability_property_exists(self) -> None:
        config = HPActorClientConfig(health=_health_config())
        client = HPActorClient(config)
        self.assertIsNotNone(client.health)
        self.assertIsInstance(client.health, object)
