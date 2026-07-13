External Client SDK
===================

The ``hpactor.client`` package provides sync and async clients for HPActor
health, metrics, HTTP gateway, and CLI surfaces.  It is pure Python — it
never imports the ``_hpactor`` native extension.

.. code-block:: python

    from hpactor.client import (
        AsyncHPActorClient,
        HealthClientConfig,
        HPActorClientConfig,
        HttpEndpointConfig,
        MetricsClientConfig,
    )

    endpoint = HttpEndpointConfig(base_url="https://node.example")
    config = HPActorClientConfig(
        health=HealthClientConfig(endpoint=endpoint),
        metrics=MetricsClientConfig(endpoint=endpoint),
    )

    async with AsyncHPActorClient(config) as client:
        readiness = await client.health.readiness()
        metrics = await client.metrics.scrape()

Configuration
-------------

All configuration types are frozen, validated, and secret-redacted.

.. py:class:: HttpEndpointConfig

   .. py:attribute:: base_url: str
   .. py:attribute:: verify: bool | ssl.SSLContext
   .. py:attribute:: follow_redirects: bool
   .. py:attribute:: timeouts: HttpTimeouts
   .. py:attribute:: limits: HttpLimits
   .. py:attribute:: retry: RetryPolicy

.. py:class:: HttpTimeouts

   .. py:attribute:: connect: float = 5.0
   .. py:attribute:: read: float = 30.0
   .. py:attribute:: write: float = 30.0
   .. py:attribute:: pool: float = 5.0

.. py:class:: HttpLimits

   .. py:attribute:: max_keepalive_connections: int = 20
   .. py:attribute:: max_connections: int = 100
   .. py:attribute:: max_response_bytes: int = 16 MiB

.. py:class:: RetryPolicy

   Opt-in.  Applies only to explicitly idempotent requests.

   .. py:attribute:: attempts: int = 1  (max 3)
   .. py:attribute:: initial_delay: float = 0.1
   .. py:attribute:: max_delay: float = 2.0
   .. py:attribute:: retry_statuses: frozenset[int] = {429, 502, 503, 504}

Health Client
-------------

.. py:class:: HealthClient
.. py:class:: AsyncHealthClient

   .. py:method:: liveness() -> HealthResult
   .. py:method:: readiness() -> HealthResult
   .. py:method:: require_live() -> HealthResult
   .. py:method:: require_ready() -> HealthResult

   ``require_live()`` and ``require_ready()`` raise ``HealthCheckFailed``
   unless the state is ``HEALTHY``.

.. py:class:: HealthResult

   .. py:attribute:: probe: HealthProbe
   .. py:attribute:: state: HealthState
   .. py:attribute:: http_status: int
   .. py:attribute:: checks: tuple[HealthCheck, ...]
   .. py:attribute:: raw_body: bytes

.. py:class:: HealthState

   Enum: ``HEALTHY``, ``DEGRADED``, ``UNHEALTHY``, ``UNKNOWN``.

Metrics Client
--------------

.. py:class:: MetricsClient
.. py:class:: AsyncMetricsClient

   .. py:method:: scrape(*, etag: str | None = None) -> MetricsSnapshot | MetricsNotModified

   Returns raw OpenMetrics / Prometheus exposition text.  Conditional
   GET with ``etag``; a ``304`` response returns ``MetricsNotModified``.

Gateway Client
--------------

.. py:class:: GatewayClient
.. py:class:: AsyncGatewayClient

   .. py:method:: request(method, path, *, idempotent=None, allow_absolute_url=False, **httpx_options) -> httpx.Response

      Send an arbitrary HTTP request.  Responses are fully buffered and
      bounded.  Absolute URLs are rejected unless ``allow_absolute_url=True``
      is set explicitly.

   .. py:method:: request_checked(method, path, **options) -> httpx.Response

      Like ``request``, but raises ``HttpResponseError`` on non-2xx.

   .. py:method:: get(path, **options)
   .. py:method:: post(path, **options)
   .. py:method:: put(path, **options)
   .. py:method:: patch(path, **options)
   .. py:method:: delete(path, **options)

CLI Client
----------

.. py:class:: CliClient
.. py:class:: AsyncCliClient

   Communicates over Unix-domain sockets or TCP using the HPAC framing
   protocol (ASCII ``HPAC``, big-endian 32-bit payload length, protobuf
   body — byte-compatible with the C++ ``CliProtoServerActor``).

   .. py:method:: execute(path, *, params=None, args=(), fmt="pretty") -> CliResult

      Execute a command-tree path.  Raises ``CliCommandError`` if the
      server returns ``is_error=True``.

   .. py:method:: inspect(actor_id) -> InspectStateReply
   .. py:method:: kill(actor_id, *, force=False) -> KillReply
   .. py:method:: quarantine(actor_id, reason="") -> QuarantineReply
   .. py:method:: unquarantine(actor_id) -> QuarantineReply
   .. py:method:: list_actors(shard_index=0, offset=0, limit=100, filter_str="") -> ListActorsReply
   .. py:method:: system_stats() -> SystemStatsReply
   .. py:method:: memory_stats(actor_id=0) -> MemoryStatsReply

   .. py:method:: call(rpc_method, request, response_type) -> Message

      Forward-compatible low-level RPC for implemented methods.

.. py:class:: CliClientConfig

   .. py:attribute:: uds_path: str | None = "/tmp/hpactor/hpactor.sock"
   .. py:attribute:: host: str | None
   .. py:attribute:: port: int | None
   .. py:attribute:: connect_timeout: float = 5.0
   .. py:attribute:: request_timeout: float = 10.0
   .. py:attribute:: max_inbound_payload: int = 16 MiB
   .. py:attribute:: max_outbound_payload: int = 16 MiB
   .. py:attribute:: allow_insecure_remote_tcp: bool = False

   Non-loopback TCP requires ``allow_insecure_remote_tcp=True``.
   Loopback addresses (``127.0.0.0/8``, ``::1``, ``localhost``) are
   permitted without the opt-in.

Bundle
------

.. py:class:: HPActorClient
.. py:class:: AsyncHPActorClient

   Thin configured bundle that constructs only the requested
   capabilities:

   .. code-block:: python

       config = HPActorClientConfig(
           health=HealthClientConfig(endpoint=endpoint),
           cli=CliClientConfig(uds_path="/tmp/hpactor/hpactor.sock"),
       )
       with HPActorClient(config) as client:
           print(client.capabilities)   # {HEALTH, CLI}
           client.health.liveness()     # OK
           client.metrics.scrape()      # raises UnsupportedCapability

   .. py:attribute:: capabilities: frozenset[Capability]

   Accessing a capability not in *capabilities* raises
   ``UnsupportedCapability`` with the capability name.

Error Model
-----------

All SDK errors inherit from ``HPActorClientError``:

.. code-block:: text

    HPActorClientError
      ConfigurationError
        InsecureTransportError
      UnsupportedCapability
      ClientClosedError
      EventLoopMismatchError
      TransportError
        ConnectionError
        OperationTimeout
      ResponseLimitError
      ProtocolError
      HttpResponseError
      HealthCheckFailed
      CliCommandError

Bounds and Security
-------------------

- **Response bodies** are bounded at 16 MiB by default.  Streaming
  reads accumulate until the limit; exceeding it raises
  ``ResponseLimitError``.
- **Redirect following is off** by default.
- **TLS verification is on** by default.
- **Retries are off** by default and require an explicit
  ``RetryPolicy`` and idempotency declaration.
- **CLI commands are never retried** — the server may have executed the
  command before the response connection failed.
- **Non-loopback CLI TCP** requires ``allow_insecure_remote_tcp=True``
  and emits a warning.  The manual recommends UDS or an authenticated
  proxy for production.
- **Diagnostic payloads in exceptions** are truncated to 4 KiB.

Server-Side Requirements
------------------------

Each client expects explicit server-side configuration:

- **Health** — an HTTP listener with ``HealthHttpServer`` attached
- **Metrics** — an explicitly exposed route returning
  ``MetricsActor`` output (not an automatic ``/metrics`` listener)
- **Gateway** — ``HTTPGatewayActor`` with registered application routes
- **CLI** — ``CliProtoServerActor`` listening on UDS or trusted TCP
