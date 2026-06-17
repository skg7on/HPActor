.. _sre-integration-tracing-jaeger:

Distributed Tracing with Jaeger
================================

HPActor's W3C-compatible trace context propagates across actor
boundaries and network hops. This guide covers exporting traces to
Jaeger (or any OpenTelemetry-compatible collector) for visualization.

Trace Flow in HPActor
---------------------

.. code-block:: text

   Client ──► HTTP Gateway ──► CoordinatorActor ──► WorkerActor
                │                  │                    │
                ├─ Span A ─────────┼─ Span B ───────────┼─ Span C
                │  (server)        │  (consumer)        │  (consumer)
                │                  │  (producer)        │
                ▼                  ▼                    ▼
             TraceManager ───────────────────► Exporter ──► Jaeger

Each actor message handler creates spans automatically. The
:cpp:class:`TraceManager` collects and exports them.

Configuration
-------------

.. code-block:: toml

   [system.tracing]
   enabled = true
   sampling_rate = 0.1
   ring_buffer_capacity = 65536
   exporter = "memory"   # Built-in memory exporter for testing

   # For production, implement a custom exporter:

The built-in ``MemoryExporter`` keeps spans in memory for CLI inspection
and testing. For production, implement the :cpp:class:`TraceExporter`
interface:

.. code-block:: cpp

   class JaegerTraceExporter : public TraceExporter {
   public:
       void export_spans(std::vector<Span> spans) override {
           // Convert spans to Jaeger Thrift or OTLP format
           // Send to Jaeger collector via UDP or gRPC
       }
   };

   // Register with the trace manager:
   system.trace_manager().set_exporter(
       std::make_unique<JaegerTraceExporter>(jaeger_config));

Jaeger via OpenTelemetry Collector
----------------------------------

The recommended approach is to route HPActor traces through the
OpenTelemetry Collector, which can forward to Jaeger (and many other
backends):

.. code-block:: text

   HPActor ──► OTLP gRPC ──► OpenTelemetry Collector ──► Jaeger
                                       │
                                       ├──► Prometheus (span metrics)
                                       └──► Logging exporter (debug)

OpenTelemetry Collector Configuration:

.. code-block:: yaml

   # otel-collector-config.yaml
   receivers:
     otlp:
       protocols:
         grpc:
           endpoint: 0.0.0.0:4317

   exporters:
     jaeger:
       endpoint: jaeger:14250
       tls:
         insecure: true

     logging:
       loglevel: debug

   service:
     pipelines:
       traces:
         receivers: [otlp]
         exporters: [jaeger, logging]

Then implement an OTLP exporter in HPActor (or use a sidecar that reads
the memory exporter via CLI and forwards).

Analyzing Traces in Jaeger
--------------------------

Key queries in Jaeger UI:

- **Service**: ``hpactor``
- **Operation**: filter by span name (e.g., ``process OrderRequest``)
- **Tags**: ``actor.type=worker``, ``message.type=6``
- **Duration**: find slow handlers: ``duration > 100ms``

Trace Attributes
~~~~~~~~~~~~~~~~

Each span includes these attributes for filtering:

.. list-table::
   :header-rows: 1

   * - Attribute
     - Description
   * - ``actor.id``
     - ActorId string (e.g., ``actor:42``)
   * - ``actor.type``
     - Actor type name (e.g., ``worker``)
   * - ``message.type``
     - TypeTag of the processed message
   * - ``message.id``
     - Correlated MessageId
   * - ``node.id``
     - Node identifier for cross-node traces

Debugging with Traces
---------------------

Common debugging scenarios:

**Finding slow actors:**

.. code-block:: text

   In Jaeger: filter by service=hpactor, sort by duration descending.
   Long spans indicate slow message handlers.

**Tracing message flow:**

.. code-block:: text

   Search by trace_id (from log line or API response header).
   The trace shows every actor that touched the message.

**Identifying missing handlers:**

.. code-block:: text

   Look for spans with status=ERROR. These indicate messages that
   could not be delivered or processed.

Sampling Strategy
-----------------

.. list-table::
   :header-rows: 1

   * - Strategy
     - Config
     - Use Case
   * - **Always off**
     - ``sampling_rate = 0``
     - Disable tracing entirely
   * - **Probability**
     - ``sampling_rate = 0.1`` (10%)
     - Production: sample enough to see patterns
   * - **Always on**
     - ``sampling_rate = 1.0``
     - Development, debugging, low-traffic systems

Parent-based sampling ensures that if any span in a trace is sampled,
all spans in that trace are sampled (complete traces, never partial).

For health and readiness checks that participate in tracing, see
:doc:`/monitoring/health`.
