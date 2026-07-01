# Distributed Tracing - Core Concept and Architecture Design

## 1. Executive Summary

Distributed tracing gives HPActor a causal view of one request as it crosses
HTTP ingress, local actor mailboxes, remote actor proxies, RPC calls, spawn
requests, and transport frames across a cluster. The core design is to carry an
OpenTelemetry-compatible trace context in the message envelope rather than in
user payloads. Actors keep handling `TypedMessage` values, while the framework
propagates trace IDs and records spans around message delivery and actor
execution.

**Key Design Decisions:**

- **Envelope-level propagation**: `TypedMessage` carries a `TraceContext`, and
  `ActorMsgFrame` serializes it on the wire. User protobuf payloads remain
  unchanged.
- **Actor handling spans by default**: each sampled message receive creates one
  span for the actor's handling work. Optional producer spans can be enabled for
  remote sends or all sends.
- **Out-of-band export**: completed spans are written to a bounded lock-free
  ring buffer. A tracing drain exports them through OpenTelemetry OTLP, JSON, or
  test sinks outside the actor hot path.
- **W3C Trace Context compatibility**: HTTP ingress extracts `traceparent` and
  `tracestate`; HTTP egress injects them. Remote actor frames use the same
  trace ID, span ID, flags, and bounded trace state fields.
- **Parent-based sampling**: sampled traces record spans; unsampled traces still
  propagate context so downstream services can correlate logs and choose their
  own policy.

The first implementation should add tracing as an opt-in runtime feature with a
compile-time guard, similar to actor metrics and CLI support.

---

## 2. Problem Statement

HPActor already supports local actors, remote actor references, service
discovery, async RPC, remote spawn, HTTP ingress, metrics, and structured
logging. Once a request crosses more than a few actors, operators cannot answer:

1. Which ingress request caused this actor message?
2. Which actors handled the request, and in what order?
3. Which hop added latency: mailbox wait, actor processing, network send, RPC
   retry, or remote spawn?
4. Did a response come from the same trace that created the request?
5. Which logs and metrics correspond to one failing request?

Message payloads are the wrong place for this data because HPActor supports many
user-defined protobuf types and system messages. The trace context must be a
framework-level envelope concern.

---

## 3. Goals and Non-Goals

### Goals

- Propagate trace context through local sends, replies, scheduled messages,
  remote sends, RPC, remote spawn, HTTP ingress, and HTTP egress.
- Represent trace context in a format compatible with OpenTelemetry and W3C
  Trace Context.
- Keep disabled tracing close to a null branch on hot paths.
- Avoid allocations on sampled hot paths except where already required by
  existing message ownership boundaries.
- Export spans asynchronously through bounded buffers with explicit dropped-span
  accounting.
- Make trace/span IDs available to metrics and logging without coupling those
  systems to an exporter.
- Preserve backward compatibility with older protobuf frames that do not carry
  tracing fields.

### Non-Goals

- Do not require the OpenTelemetry C++ SDK in the core actor runtime.
- Do not add tracing fields to user payload protobuf messages.
- Do not make every actor send synchronous or blocking for export.
- Do not propagate arbitrary baggage by default. Baggage can carry sensitive or
  high-cardinality data and should be a later opt-in feature.
- Do not guarantee a total order across nodes. Tracing records causal parentage
  and local timestamps, not a globally synchronized timeline.

---

## 4. Design Alternatives

| Option | Summary | Trade-offs |
|--------|---------|------------|
| Payload-level trace fields | Require each user protobuf to include trace IDs. | Simple per-message inspection, but breaks user APIs, misses system messages, and cannot enforce consistency. Not recommended. |
| Envelope propagation with native exporter | Add trace context to `TypedMessage` and `ActorMsgFrame`; emit native span records to an async exporter. | Fits existing message architecture, keeps payloads clean, and avoids hard dependency on the OpenTelemetry SDK. Recommended. |
| Direct OpenTelemetry SDK in every actor path | Wrap actor execution with OTel SDK spans directly. | Rich ecosystem support, but adds dependency, allocation, exceptions/RTTI risk, and exporter coupling to hot paths. Useful only as an optional bridge. |

The recommended architecture is envelope propagation with a native HPActor
tracing service and an OTLP exporter.

---

## 5. Core Concepts

### Trace

A trace is the end-to-end causal graph for one logical request. Every hop keeps
the same `TraceId`.

### Span

A span describes one timed unit of work. HPActor should create spans for:

- HTTP ingress request handling.
- Actor message receive and behavior execution.
- Remote send producer work when producer spans are enabled.
- RPC calls and retry attempts.
- Remote spawn request handling.
- HTTP egress client calls.

### Span Context

A span context is the small propagation token carried in message envelopes:

```cpp
struct TraceId {
    std::array<uint8_t, 16> bytes{};
};

struct SpanId {
    std::array<uint8_t, 8> bytes{};
};

struct TraceFlags {
    uint8_t value = 0; // bit 0 = sampled, W3C-compatible
};

struct TraceContext {
    TraceId trace_id;
    SpanId span_id;        // parent/current span context for downstream work
    TraceFlags flags;
    uint8_t version = 0;
    uint8_t state_len = 0;
    std::array<char, 256> tracestate{}; // bounded, optional

    bool valid() const noexcept;
    bool sampled() const noexcept;
};
```

HPActor already has a small initial `TraceContext` type in
`include/hpactor/types/types.hpp`. The tracing feature should expand that type
to a W3C-compatible representation while preserving the same public concept.

### Span Link

Some actor flows are causal but not strictly nested. A reply, retry, or fan-out
message can attach a span link to the original message ID or parent span ID. The
first version can record links only for RPC retries and replies; richer fan-in
linking can be added in a follow-up without changing propagation.

---

## 6. Architecture Overview

```text
HTTP ingress / ActorContext root / RPC caller
        |
        | extract or create TraceContext
        v
ActorContext
  send(), reply(), schedule(), rpc(), http_*()
        |
        | attach parent context to TypedMessage
        v
TypedMessage
  TypeTag + payload + sender + TraceContext
        |
        +---------------- local ----------------+
        |                                       |
        v                                       v
ActorSystem::deliver_local()             ActorProxy::send()
        |                                       |
        v                                       | serialize TraceContext
MPSCActorMailbox                           ActorMsgFrame.trace_context
        |                                       |
        v                                       v
EventBasedActor::receive()             TcpTransport / ConnectionPool
        |                                       |
        | start actor handling span             v
        |                               remote ActorSystem::deliver_remote()
        |                                       |
        +--------------- same receive path -----+
        |
        | completed sampled span
        v
TraceRingBuffer
        |
        | async drain
        v
TraceManager
  sampler, id generator, processors, exporters
        |
        v
OTLP / JSON / memory sink / future CLI trace view
```

### Ownership Model

```text
ActorSystem
  owns TraceManager when Config.tracing.enabled = true
    owns TraceConfig
    owns TraceIdGenerator
    owns Sampler
    owns TraceRingBuffer<SpanRecord>
    owns SpanProcessor chain
    owns SpanExporter

ActorContext
  stores current incoming TraceContext while actor handles a message
  uses TraceManager facade for send/reply/root span operations

TypedMessage
  owns optional TraceContext sidecar

ActorMsgFrame
  serializes TraceContext across process and node boundaries
```

The tracing data path is a system service, not a user actor. It must be able to
record mailbox, scheduler, and network events even when actor mailboxes are
backed up.

---

## 7. Message Envelope Integration

### `TypedMessage`

`TypedMessage` should grow an optional trace sidecar:

```cpp
class TypedMessage {
public:
    bool has_trace_context() const noexcept;
    const TraceContext& trace_context() const noexcept;
    void set_trace_context(const TraceContext& ctx) noexcept;
    void clear_trace_context() noexcept;

private:
    TraceContext trace_context_{};
    bool has_trace_context_ = false;
};
```

This keeps trace propagation independent of the serialized payload and works for
system messages, typed protobuf messages, HTTP-converted messages, and raw
`StreamBuffer` messages.

### `ActorContext::send`

When an actor sends a message:

1. Stamp the sender address as today.
2. If the message already has an explicit trace context, keep it.
3. Otherwise, copy the current actor handling span context into the message.
4. If there is no current context and tracing is configured to create local
   roots, create a new root context and apply sampling.
5. Optionally emit a producer span for the send operation.

Replies use the current handling span as parent so a trace shows:

```text
ingress span -> ActorA receive -> ActorB receive -> ActorA reply receive
```

### `ActorSystem::deliver_local`

Local delivery should not modify trace context. It only moves the
`TypedMessage` into the target mailbox. The target actor creates its receive
span when it dequeues and handles the message.

### `EventBasedActor::receive`

`receive()` becomes the boundary for actor handling spans:

1. Read `msg.trace_context()` if present.
2. Create a child span ID for this actor receive when sampled.
3. Set `ActorContext` current trace context to the new span context for the
   duration of behavior dispatch.
4. Invoke system message handling or user behavior.
5. Emit a completed `SpanRecord` with status and duration.
6. Restore the previous context before returning.

This mirrors the existing metrics instrumentation point for processing latency
and gives all actor code access to the active trace through `context()`.

---

## 8. Wire Protocol Integration

`protos/hpactor/frame.proto` should add a trace message and field:

```proto
message PbTraceContext {
  bytes trace_id = 1;      // exactly 16 bytes when present
  bytes span_id = 2;       // exactly 8 bytes when present
  uint32 flags = 3;        // W3C trace flags; bit 0 sampled
  string tracestate = 4;   // bounded by TraceConfig.max_tracestate_len
}

message ActorMsgFrame {
  hpactor.PbActorAddress sender = 1;
  hpactor.PbActorAddress receiver = 2;
  uint32 type_tag = 3;
  uint64 message_id = 4;
  uint32 flags = 5;
  bytes payload = 6;
  PbTraceContext trace_context = 7;
}
```

Proto3 preserves backward compatibility:

- New nodes can read old frames with no trace context.
- Old nodes ignore field 7 and deliver the message without propagation.
- Mixed-version clusters lose tracing across old nodes but keep message
  delivery semantics.

### Remote Send

`ActorProxy::send()` serializes `TypedMessage::trace_context()` into
`ActorMsgFrame.trace_context` when present.

### Remote Receive

`ActorSystem::deliver_remote()` deserializes `trace_context` into the new
`TypedMessage` before calling `deliver_local()`.

### RPC and Spawn

`RpcChannel::send_request()` should copy the caller's trace context into the
request frame. Responses inherit the active server/request handling context when
available, fall back to the inbound request context otherwise, and keep the same
`message_id` so the response can be correlated with the request span.

`SpawnReceiver::handle_spawn_request()` should treat a remote spawn request like
an actor receive span. The spawn response inherits the active spawn handling
context when available, with the inbound request context as the fallback.

---

## 9. HTTP and OpenTelemetry Propagation

### HTTP Ingress

`HTTPGatewayActor` extracts W3C headers:

```text
traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
tracestate: vendor=value
```

If `traceparent` is valid, the gateway uses it as the parent context. If it is
missing and tracing is enabled, the gateway creates a root trace and applies the
configured sampler. The actor message generated from the HTTP request receives
that context before local delivery.

### HTTP Egress

`ActorContext::http_get()`, `http_post()`, `http_put()`, and `http_delete()`
inject the active trace context into outgoing HTTP headers. Egress calls should
record client spans when sampled.

### OpenTelemetry Export

HPActor should export spans through a native OTLP bridge rather than calling an
OpenTelemetry SDK directly on actor hot paths:

```text
SpanRecord ring buffer
  -> BatchSpanProcessor
  -> OtlpHttpExporter
  -> collector /v1/traces
```

The core trace model maps cleanly to OpenTelemetry:

| HPActor Field | OpenTelemetry Field |
|---------------|---------------------|
| `TraceId` | `trace_id` |
| `SpanId` | `span_id` |
| parent `SpanId` | `parent_span_id` |
| actor receive span | `SpanKind::CONSUMER` |
| actor send producer span | `SpanKind::PRODUCER` |
| HTTP ingress | `SpanKind::SERVER` |
| HTTP egress / RPC client | `SpanKind::CLIENT` |
| spawn handling | `SpanKind::INTERNAL` or `CONSUMER` |

Recommended resource attributes:

| Attribute | Value |
|-----------|-------|
| `service.name` | `TraceConfig.service_name` |
| `hpactor.node.endpoint` | `ActorSystem::endpoint()` |
| `hpactor.discovery.backend` | active discovery backend name |
| `telemetry.sdk.name` | `hpactor-native` |

Recommended span attributes:

| Attribute | Source |
|-----------|--------|
| `hpactor.actor.id` | receiver actor ID |
| `hpactor.actor.type` | `AbstractActor::type_name()` |
| `hpactor.message.type_tag` | `TypedMessage::type_id()` |
| `hpactor.message.id` | `ActorMsgFrame.message_id` when available |
| `hpactor.sender.actor_id` | `TypedMessage::sender_address()` |
| `hpactor.receiver.actor_id` | target actor ID |
| `hpactor.remote.endpoint` | remote actor endpoint |
| `hpactor.mailbox.wait_ns` | optional enqueue-to-dequeue latency |
| `hpactor.worker.id` | scheduler worker ID when available |
| `rpc.retry_count` | RPC retry spans |
| `http.method`, `http.route`, `http.status_code` | HTTP gateway/client |

---

## 10. Trace Manager

### Components

```text
TraceManager
  TraceConfig
  TraceIdGenerator
  Sampler
    AlwaysOffSampler
    AlwaysOnSampler
    TraceIdRatioSampler
    ParentBasedSampler
  TraceRingBuffer
  SpanProcessor
    SimpleSpanProcessor
    BatchSpanProcessor
  SpanExporter
    NoopExporter
    JsonFileExporter
    OtlpHttpExporter
    MemoryExporter for tests
```

### Span Record

The hot path should emit one completed fixed-size record per sampled span:

```cpp
struct SpanRecord {
    TraceId trace_id;
    SpanId span_id;
    SpanId parent_span_id;
    ActorId actor_id;
    ActorId sender_actor_id;
    TypeTag type_tag;
    uint64_t message_id;
    uint64_t start_ns;
    uint64_t end_ns;
    uint32_t flags;
    uint16_t span_kind;
    uint16_t status_code;
    uint32_t attribute_mask;
};
```

The exporter resolves dynamic strings such as actor type names and endpoint text
outside the actor hot path. If it cannot resolve a label, it emits the numeric
ID only.

### Failure Behavior

- If the ring buffer is full, drop the span and increment
  `hpactor_tracing_spans_dropped_total`.
- If an exporter fails, retry with bounded exponential backoff in the drain
  thread.
- If the exporter queue remains full, drop oldest batches and expose
  `hpactor_tracing_export_batches_dropped_total`.
- Actor execution, mailbox enqueue/dequeue, scheduler dispatch, and network
  processing must never block on tracing export.

---

## 11. Sampling

Sampling decisions happen when a root context is created or when an inbound
context has no sampled flag.

| Sampler | Behavior |
|---------|----------|
| `always_off` | Propagate contexts but record no spans. |
| `always_on` | Record every span. Useful for tests and small systems. |
| `trace_id_ratio` | Deterministically sample a percentage using the trace ID. |
| `parent_based_trace_id_ratio` | Respect inbound sampled decisions; otherwise apply ratio. Recommended default. |

Unsampled messages still carry `TraceContext`. This is important because a
downstream service may run a different sampler, and logs can still include
trace IDs for correlation.

---

## 12. Configuration

### CMake

```bash
cmake -DENABLE_ACTOR_TRACING=ON ..
```

The default should be `ON` for building the code and `enabled = false` at
runtime, matching the CLI pattern.

### Runtime Config

```cpp
namespace hpactor::tracing {

struct TraceConfig {
    bool enabled = false;
    bool propagate_unsampled = true;
    uint32_t ring_buffer_capacity = 65536;
    std::string service_name = "hpactor";
    std::string propagation = "w3c";
    std::string sampler = "parent_based_trace_id_ratio";
    double sample_ratio = 0.01;
    std::string exporter = "otlp_http";
    std::string otlp_endpoint = "http://127.0.0.1:4318/v1/traces";
    std::chrono::milliseconds export_interval{500};
    uint32_t max_export_batch_size = 512;
    uint16_t max_tracestate_len = 256;
    bool record_actor_receive_spans = true;
    bool record_remote_producer_spans = true;
    bool record_local_producer_spans = false;
    bool record_payload_size = true;
};

} // namespace hpactor::tracing
```

`hpactor::Config` owns `tracing::TraceConfig tracing;`.

### TOML

```toml
[system.tracing]
enabled = true
service_name = "checkout-actors"
propagation = "w3c"
sampler = "parent_based_trace_id_ratio"
sample_ratio = 0.05
ring_buffer_capacity = 131072
exporter = "otlp_http"
otlp_endpoint = "http://otel-collector:4318/v1/traces"
export_interval_ms = 500
max_export_batch_size = 512
max_tracestate_len = 256
record_actor_receive_spans = true
record_remote_producer_spans = true
record_local_producer_spans = false
record_payload_size = true
```

The topology parser, binary serializer, and AOT compiler should preserve the
same fields under `TopologyModel::SystemDef`.

---

## 13. Integration With Metrics, Logging, and CLI

### Metrics

Tracing should expose operational metrics through the existing metrics system:

| Metric | Type | Meaning |
|--------|------|---------|
| `hpactor_tracing_spans_started_total` | Counter | Sampled spans started by kind. |
| `hpactor_tracing_spans_finished_total` | Counter | Sampled spans finished by status. |
| `hpactor_tracing_spans_dropped_total` | Counter | Ring buffer overflow drops. |
| `hpactor_tracing_export_batches_total` | Counter | Export batches attempted. |
| `hpactor_tracing_export_failures_total` | Counter | Export failures by exporter. |
| `hpactor_tracing_export_latency_seconds` | Histogram | Export batch duration. |

### Logging

The logging subsystem already reserves trace/span IDs as structured fields. Once
tracing exists, `LogEvent` should copy the active `TraceContext` from
`ActorContext` or the `TypedMessage` being processed. Logs do not start spans;
they only correlate with active traces.

### CLI

The first CLI integration should be read-only:

```text
/trace show <trace_id>
/trace sample <ratio>
/trace exporters
```

The `/trace show` command can query an in-memory recent-span buffer if enabled.
It is not required for the initial tracing data path.

---

## 14. Error Handling and Edge Cases

| Scenario | Behavior |
|----------|----------|
| Missing trace context | Create a root only at configured ingress/root boundaries. Otherwise deliver normally. |
| Malformed HTTP `traceparent` | Ignore inbound context, increment a malformed counter, create a new root if enabled. |
| Invalid protobuf trace field size | Drop only the trace sidecar and deliver the message. |
| Mixed-version cluster | New fields are ignored by old nodes; trace continuity resumes after the old-node hop only if a later component creates a new root. |
| Ring buffer overflow | Drop spans, increment dropped counter, never block actors. |
| Exporter unavailable | Retry asynchronously with bounded backoff; continue recording until buffers fill. |
| Actor failure during receive | HPActor is built without exceptions; status comes from explicit error replies, failures, and system lifecycle events. |
| Actor termination while span active | Finish span with error status before `receive()` or `on_exit()` returns. |
| Clock skew across nodes | Use local monotonic timestamps for duration. Export wall-clock timestamps are best effort. |

---

## 15. Security and Cardinality

- Trace IDs and span IDs are opaque random or pseudo-random values. They must not
  encode actor IDs, endpoints, user IDs, or payload data.
- Do not record payload bytes. `record_payload_size` is safe because it records
  only byte counts.
- Keep `tracestate` bounded and validate characters before propagation.
- Do not enable baggage in v1. If baggage is added later, enforce an allowlist
  and size limits.
- Avoid high-cardinality labels in metrics. Trace attributes can contain actor
  IDs, but metrics derived from tracing should aggregate by kind/status/exporter
  unless explicitly configured otherwise.

---

## 16. Testing Strategy

### Unit Tests

- `TraceId` and `SpanId` generation rejects all-zero IDs.
- W3C `traceparent` parse/format round trips valid headers.
- Invalid `traceparent` headers are rejected without throwing.
- `TraceContext` copies through `TypedMessage` move construction and assignment.
- Samplers make deterministic decisions for fixed trace IDs.
- `TraceRingBuffer` records and reports dropped spans under overflow.

### Integration Tests

- Local actor send preserves trace ID and creates child receive spans.
- Actor reply keeps the same trace and sets the reply receiver's parent span to
  the replying actor's span.
- Remote actor send serializes and deserializes `ActorMsgFrame.trace_context`.
- RPC request, retry, and response share trace ID and message ID correlation.
- Remote spawn request and response propagate the caller trace.
- HTTP ingress extracts `traceparent` and injects context into the actor message.
- HTTP egress injects `traceparent` from the active actor context.
- Tracing disabled keeps existing message behavior and emits no spans.
- Mixed old/new frame decode without trace context delivers messages normally.

### Verification

Run the standard project suite:

```bash
cmake -S . -B build -GNinja
ninja -C build
ctest --output-on-failure
```

Add focused tests under `tests/tracing/` and wire them into CMake behind
`ENABLE_ACTOR_TRACING`.

---

## 17. Implementation Phases

### Phase 1: Core Context and Local Propagation

- Expand `TraceContext` to W3C-compatible fixed-size IDs.
- Add trace context accessors to `TypedMessage`.
- Add current trace scope to `ActorContext`.
- Propagate context through `send()`, `reply()`, and `deliver_local()`.
- Add local receive span recording with a memory exporter for tests.

### Phase 2: Wire, RPC, Spawn, and HTTP Propagation

- Add `PbTraceContext` to `frame.proto`.
- Serialize and deserialize trace context in `ActorProxy::send()` and
  `ActorSystem::deliver_remote()`.
- Propagate trace context through `RpcChannel` request/response and
  `SpawnReceiver`.
- Extract/inject W3C headers in HTTP gateway and client paths.

### Phase 3: TraceManager and Bounded Export Pipeline

- Add `TraceConfig`, `TraceManager`, samplers, `TraceRingBuffer`, span
  processors, and memory/json exporters.
- Expose dropped-span and exporter metrics.
- Integrate trace IDs into structured logging events.

### Phase 4: OTLP Export

- Add an OTLP/HTTP exporter using existing protobuf and HTTP client
  infrastructure where possible.
- Batch spans, retry failed exports, and expose exporter metrics.
- Document deployment with an OpenTelemetry Collector.

### Phase 5: CLI and Advanced Diagnostics

- Add read-only CLI inspection for recent traces if an in-memory trace cache is
  enabled.
- Add optional producer spans for all local sends.
- Add span links for fan-in/fan-out patterns and richer RPC retries.

---

## 18. Expected Developer Experience

Default actor code does not change:

```cpp
context()->send(worker, TypedMessage(TypeTag::User, payload));
context()->reply(TypedMessage(TypeTag::User, response));
```

Tracing-aware code can inspect the active context:

```cpp
auto trace = context()->current_trace_context();
if (trace.valid()) {
    HPACTOR_LOG_INFO(actor, "processing order", trace);
}
```

HTTP clients and gateways automatically bridge W3C headers:

```cpp
context()->http_post("https://inventory.service/reserve", body);
```

Operators enable tracing with TOML and point HPActor at an OpenTelemetry
Collector. The trace view then shows the request moving through actors,
mailboxes, remote nodes, RPC retries, and external HTTP calls.

---

## 19. Definition of Done

The distributed tracing feature is complete when:

- Trace context propagates across local actor sends, replies, remote frames,
  RPC, remote spawn, HTTP ingress, and HTTP egress.
- Sampled actor receive spans are exported asynchronously through at least one
  test sink and one production sink.
- OTLP export works with an OpenTelemetry Collector without requiring actor code
  changes.
- Metrics expose dropped spans and exporter health.
- Logs can include trace and span IDs when a trace is active.
- Tracing disabled preserves current behavior and performance expectations.
- The full test suite passes with tracing enabled and disabled.
