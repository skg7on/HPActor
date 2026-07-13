# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Configuration validation tests for the external SDK."""

import unittest
from dataclasses import FrozenInstanceError

from hpactor.client import (
    CliClientConfig,
    GatewayClientConfig,
    HealthClientConfig,
    HPActorClientConfig,
    HttpEndpointConfig,
    HttpLimits,
    HttpTimeouts,
    InsecureTransportError,
    MetricsClientConfig,
    RetryPolicy,
)


class ConfigTest(unittest.TestCase):
    def test_defaults_are_bounded_and_immutable(self) -> None:
        endpoint = HttpEndpointConfig(base_url="https://node.example")
        self.assertEqual(
            endpoint.timeouts,
            HttpTimeouts(5.0, 30.0, 30.0, 5.0),
        )
        self.assertEqual(
            endpoint.limits.max_response_bytes,
            16 * 1024 * 1024,
        )
        self.assertFalse(endpoint.follow_redirects)
        with self.assertRaises(FrozenInstanceError):
            endpoint.follow_redirects = True  # type: ignore[misc]

    def test_remote_cli_requires_explicit_opt_in(self) -> None:
        with self.assertRaises(InsecureTransportError):
            CliClientConfig(uds_path=None, host="node.example", port=7777)
        config = CliClientConfig(
            uds_path=None,
            host="node.example",
            port=7777,
            allow_insecure_remote_tcp=True,
        )
        self.assertEqual(config.port, 7777)

    def test_localhost_tcp_permitted_without_opt_in(self) -> None:
        for host in ("127.0.0.1", "::1", "localhost"):
            config = CliClientConfig(uds_path=None, host=host, port=7777)
            self.assertEqual(config.host, host)

    def test_http_endpoint_rejects_userinfo(self) -> None:
        with self.assertRaises(ValueError):
            HttpEndpointConfig(base_url="https://user:pass@node.example")

    def test_health_config_default_paths(self) -> None:
        endpoint = HttpEndpointConfig(base_url="https://node.example")
        config = HealthClientConfig(endpoint=endpoint)
        self.assertEqual(config.liveness_path, "/healthz")
        self.assertEqual(config.readiness_path, "/readyz")

    def test_metrics_config_default_path(self) -> None:
        endpoint = HttpEndpointConfig(base_url="https://node.example")
        config = MetricsClientConfig(endpoint=endpoint)
        self.assertEqual(config.metrics_path, "/metrics")

    def test_bundle_requires_at_least_one_capability(self) -> None:
        with self.assertRaises(ValueError):
            HPActorClientConfig()

    def test_retry_defaults(self) -> None:
        policy = RetryPolicy()
        self.assertEqual(policy.attempts, 1)
        self.assertEqual(policy.initial_delay, 0.1)
        self.assertEqual(policy.max_delay, 2.0)
        self.assertIn(429, policy.retry_statuses)

    def test_retry_attempts_bounded_1_to_3(self) -> None:
        RetryPolicy(attempts=3)
        with self.assertRaises(ValueError):
            RetryPolicy(attempts=0)
        with self.assertRaises(ValueError):
            RetryPolicy(attempts=4)

    def test_timeout_values_must_be_positive_finite(self) -> None:
        with self.assertRaises(ValueError):
            HttpTimeouts(connect=-1.0)
        with self.assertRaises(ValueError):
            HttpTimeouts(read=float("inf"))

    def test_limits_must_be_positive(self) -> None:
        with self.assertRaises(ValueError):
            HttpLimits(max_response_bytes=0)
        with self.assertRaises(ValueError):
            HttpLimits(max_keepalive_connections=-1)
