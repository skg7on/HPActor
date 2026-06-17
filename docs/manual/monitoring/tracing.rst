.. _monitoring-tracing:

Distributed Tracing
===================

HPActor implements W3C-compatible distributed tracing with automatic
propagation across actor boundaries and network hops.

Architecture
------------

.. code-block:: text

   ┌──────────────────────────────────────────┐
   │              TraceManager                 │
   │  ┌──────────┐  ┌───────────┐             │
   │  │  Sampler  │  │RingBuffer │──► Drain ──► Exporter │
   │  └──────────┘  └───────────┘             │
   └──────────────────────────────────────────┘
            │
   ┌────────▼──────┐  ┌──────▼─────┐
   │ Actor A Span  │  │Actor B Span│
   │ (consumer)    │──► (producer) │
   └───────────────┘  └────────────┘

Traces follow messages through the system:
1. When Actor A receives a message, a **consumer span** starts (RAII guard).
2. When Actor A sends to Actor B, the current trace context propagates.
3. Actor B's receive creates a new span with Actor A's span as parent.
4. Spans are flushed to the ring buffer and exported asynchronously.

W3C Trace Context
-----------------

Every message carries a :cpp:class:`TraceContext`:

.. code-block:: cpp

   struct TraceContext {
       uint8_t trace_id[16];   // 16-byte TraceId (W3C format)
       uint8_t span_id[8];     // 8-byte SpanId
       uint8_t trace_flags;    // Sampled flag, etc.
   };

This is compatible with OpenTelemetry, Jaeger, and Zipkin propagation
formats (via translation at the trace collector).

Span Types
----------

.. list-table::
   :header-rows: 1

   * - Kind
     - Description
     - Example
   * - ``consumer``
     - Actor receiving and processing a message
     - Message handler execution
   * - ``producer``
     - Actor sending a message
     - ``context()->send()`` call
   * - ``client``
     - Outbound RPC request
     - ``context()->ask()``
   * - ``server``
     - Inbound RPC request handler
     - ``on_request`` handler
   * - ``internal``
     - Internal operation within an actor
     - Background cleanup, compaction

Span Structure
--------------

.. code-block:: cpp

   struct Span {
       TraceId trace_id;
       SpanId span_id;
       SpanId parent_span_id;
       std::string name;
       SpanKind kind;
       Clock::time_point start_time;
       Clock::duration duration;
       SpanStatus status;
       std::map<std::string, std::string> attributes;
       std::vector<SpanEvent> events;
       std::vector<SpanLink> links;
   };

Attributes include:
- ``actor.id`` — ActorId string.
- ``actor.type`` — actor type name.
- ``message.type`` — TypeTag of the processed message.
- ``message.id`` — correlated MessageId.

RAII Span Guard
---------------

Spans are managed automatically:

.. code-block:: cpp

   // In EventBasedActor::receive():
   SpanGuard guard(trace_context, "process OrderRequest",
                   SpanKind::consumer);
   // ... message processing ...
   // Span ends when guard goes out of scope.

No manual span management is needed for normal message processing.

Sampling
--------

Parent-based sampling with configurable rate:

.. code-block:: toml

   [system.tracing]
   enabled = true
   sampling_rate = 0.1              # 10% of new traces
   ring_buffer_capacity = 65536
   exporter = "memory"              # "memory" or custom

Sampling decision:
- If the incoming message has ``TraceFlags::sampled`` set → sample (respects upstream).
- Otherwise → random sampling at ``sampling_rate``.
- All spans in a sampled trace are exported; none in an unsampled trace.

Remote Propagation
------------------

For cross-node messaging, the trace context is serialized into the
``PbTraceContext`` field of the wire ``Frame``:

.. code-block:: protobuf

   message PbTraceContext {
       bytes trace_id = 1;    // 16 bytes
       bytes span_id = 2;     // 8 bytes
       uint32 trace_flags = 3;
   }

The receiving node creates a new span with the propagated trace context
as parent — the trace continues seamlessly across node boundaries.

Configuration
-------------

.. code-block:: toml

   [system.tracing]
   enabled = true
   sampling_rate = 0.1
   exporter = "memory"
   ring_buffer_capacity = 65536

Compile-time disable:

.. code-block:: bash

   cmake -DENABLE_ACTOR_TRACING=OFF ..

CLI Inspection
--------------

.. code-block:: text

   /trace spans [actor_id]    # List recent spans
   /trace span <span_id>       # Show span details

For trace visualization and export, see
:doc:`/sre-integration/tracing-jaeger`.
