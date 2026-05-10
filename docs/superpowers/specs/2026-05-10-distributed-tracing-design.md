# Distributed Tracing Design Specification

**Date:** 2026-05-10
**Status:** Draft
**Author:** HPActor Team
**Source Architecture:** `docs/architecture/actor/distributed-tracing-design.md`

---

## Overview

Add a native distributed tracing subsystem for HPActor so one logical request
can be followed across HTTP ingress, local actor sends, replies, scheduled
messages, remote actor sends, RPC calls, remote spawn, and outbound HTTP calls.
The feature propagates OpenTelemetry-compatible trace context at the framework
message-envelope layer and exports sampled spans through a bounded asynchronous
pipeline.

The core principle is unchanged from the architecture document: tracing belongs
in `TypedMessage` and `ActorMsgFrame`, not in user protobuf payloads. Actor code
keeps using the same `context()->send(...)`, `context()->reply(...)`,
`context()->rpc(...)`, and `context()->http_*()` APIs while the runtime carries
trace context and records spans around framework boundaries.

## Goals

- **Envelope-level propagation**: carry `TraceContext` in `TypedMessage` and
  protobuf `ActorMsgFrame`.
- **W3C compatibility**: parse and format `traceparent` and bounded
  `tracestate` according to W3C Trace Context.
- **Actor receive spans**: create one consumer span for each sampled actor
  message receive, including system messages that are delivered through the
  actor mailbox.
- **Cross-boundary propagation**: preserve trace context through local sends,
  replies, scheduling, remote sends, RPC, remote spawn, HTTP ingress, and HTTP
  egress.
- **Bounded export path**: write completed spans to a lock-free ring buffer and
  export them outside actor execution.
- **Minimal disabled overhead**: when tracing is disabled, hot paths do one null
  check or compile out entirely under `ENABLE_ACTOR_TRACING=OFF`.
- **No new core runtime dependency on OpenTelemetry SDK**: HPActor owns the hot
  path model and can export OTLP directly or through a thin optional bridge.
- **No exceptions, no RTTI**: all parsing and export errors use `result<T>` or
  explicit status codes.

## Non-Goals

- No trace fields in user payload protobuf messages.
- No synchronous span export from actor, mailbox, scheduler, transport, or HTTP
  paths.
- No baggage propagation in the first version.
- No full CLI trace browser in the first version. The data model must allow it,
  but CLI commands can follow after export works.
- No promise that timestamps from different nodes form a total order. Durations
  use local monotonic clocks.

---

## Existing Extension Points

The design relies on current HPActor boundaries:

| Area | Current File | Current Role | Tracing Change |
|------|--------------|--------------|----------------|
| Base trace type | `include/hpactor/types/types.hpp` | Contains the initial small `TraceContext` type. | Expand it to W3C-sized IDs and flags. |
| Message envelope | `include/hpactor/actor/typed_message.hpp` | Owns `TypeTag`, payload, parsed local message, sender address. | Add optional `TraceContext` sidecar and move propagation. |
| Actor send/reply | `src/actor/actor_context.cpp` | Stamps sender and resolves local/remote refs. | Attach or preserve trace context before send/reply/schedule/RPC/HTTP. |
| Actor receive | `src/actor/event_based_actor.cpp` | Dispatches system, CLI, proto, and behavior handlers. | Start/finish receive spans and install current trace scope. |
| Remote frame | `protos/hpactor/frame.proto` | Serializes sender, receiver, type tag, message id, flags, payload. | Add `PbTraceContext trace_context = 7`. |
| Frame conversion | `src/net/frame_protobuf.cpp` | Converts actor addresses to/from protobuf. | Add trace context conversion helpers. |
| Remote send | `src/ref/actor_proxy.cpp` | Builds `WireFrame` and sends through transport. | Serialize message trace context into the frame. |
| Remote receive | `src/actor/actor_system.cpp` | Converts `WireFrame` to `TypedMessage`. | Deserialize frame trace context into the message. |
| RPC | `include/hpactor/rpc/rpc_channel.hpp`, `src/rpc/rpc_channel.cpp` | Tracks pending raw RPC calls and retries. | Store call trace context, write it to request frames, expose response context when needed. |
| Remote spawn | `src/actor/actor_system.cpp`, `src/actor/spawn_receiver.cpp` | Sends spawn request/response frames. | Propagate trace context on request and response frames. |
| HTTP ingress | `src/actor/http_gateway_actor.cpp` | Builds actor messages from HTTP requests and correlates replies. | Extract W3C headers, attach trace context, preserve it through request-id payload wrapping. |
| HTTP egress | `src/actor/actor_context.cpp`, `src/net/http_client.cpp` | Sends outbound HTTP requests. | Inject W3C headers and record client spans. |
| Ring buffer | `include/hpactor/metrics/metrics_ring_buffer.hpp` | Generic bounded MPSC buffer already available. | Reuse as `MpscRingBuffer<SpanRecord>`. |
| Config | `include/hpactor/core/actor_system.hpp`, `include/hpactor/config/topology_model.hpp`, `src/config/toml_parser.cpp` | Runtime and TOML config. | Add tracing config and late apply support. |

---

## Proposed Directory Layout

```text
include/hpactor/tracing/
    trace_config.hpp              // TraceConfig runtime options
    trace_ids.hpp                 // TraceId, SpanId helper functions if split from types.hpp
    trace_context_parser.hpp      // W3C traceparent/tracestate parse/format
    trace_manager.hpp             // TraceManager facade owned by ActorSystem
    span.hpp                      // SpanKind, SpanStatus, SpanStart, SpanRecord, SpanScope
    sampler.hpp                   // Sampler interface and built-in samplers
    trace_exporter.hpp            // SpanExporter interface
    memory_exporter.hpp           // Test exporter storing spans in memory
    json_exporter.hpp             // File/stdout JSON lines exporter
    otlp_exporter.hpp             // OTLP/HTTP exporter facade

src/tracing/
    trace_context_parser.cpp
    trace_manager.cpp
    sampler.cpp
    memory_exporter.cpp
    json_exporter.cpp
    otlp_exporter.cpp

tests/tracing/
    test_trace_context.cpp
    test_w3c_trace_context.cpp
    test_sampler.cpp
    test_trace_manager.cpp
    test_trace_message_propagation.cpp
    test_trace_wire_propagation.cpp
    test_trace_http_propagation.cpp
    test_trace_config.cpp
```

`TraceContext` itself should stay available from `include/hpactor/types/types.hpp`
to avoid breaking existing includes. The implementation can either expand the
type directly in `types.hpp` or move the definition to
`include/hpactor/tracing/trace_ids.hpp` and include that file from `types.hpp`.
The important compatibility rule is that existing code including
`hpactor/types/types.hpp` can still name `hpactor::TraceContext`.

---

## Public Data Model

### TraceId and SpanId

OpenTelemetry and W3C Trace Context require a 16-byte trace ID and an 8-byte
span ID. All-zero IDs are invalid.

```cpp
namespace hpactor {

struct TraceId {
    std::array<uint8_t, 16> bytes{};

    bool valid() const noexcept;
    bool operator==(const TraceId& other) const noexcept;
};

struct SpanId {
    std::array<uint8_t, 8> bytes{};

    bool valid() const noexcept;
    bool operator==(const SpanId& other) const noexcept;
};

struct TraceFlags {
    static constexpr uint8_t kSampled = 0x01;

    uint8_t value{0};

    bool sampled() const noexcept { return (value & kSampled) != 0; }
    void set_sampled(bool enabled) noexcept;
};

} // namespace hpactor
```

Design requirements:

- `valid()` returns false only for all-zero IDs.
- IDs are byte arrays, not integers, to avoid endian ambiguity and to match
  OpenTelemetry serialization.
- Formatting functions output lowercase hex.
- Parsing functions reject wrong lengths, non-hex characters, and all-zero IDs.

### TraceContext

`TraceContext` is the sidecar propagated through message envelopes.

```cpp
namespace hpactor {

struct TraceContext {
    TraceId trace_id;
    SpanId span_id;
    TraceFlags flags;
    uint8_t version{0};
    uint16_t tracestate_len{0};
    std::array<char, 256> tracestate{};

    bool valid() const noexcept {
        return trace_id.valid() && span_id.valid();
    }

    bool sampled() const noexcept {
        return flags.sampled();
    }

    std::string_view tracestate_view() const noexcept {
        return {tracestate.data(), tracestate_len};
    }

    void clear() noexcept;
};

} // namespace hpactor
```

The fixed `tracestate` buffer avoids allocation on propagation paths. Parsing
must truncate only by rejecting over-limit input, never by silently cutting a
header, because a truncated tracestate can become semantically invalid.

### Traceparent Parse and Format

Add W3C helpers:

```cpp
namespace hpactor::tracing {

enum class TraceParseStatus : uint8_t {
    kOk,
    kMissing,
    kMalformed,
    kUnsupportedVersion,
    kInvalidTraceId,
    kInvalidSpanId,
    kTracestateTooLarge,
};

struct TraceParseResult {
    TraceParseStatus status{TraceParseStatus::kMissing};
    TraceContext context{};
};

TraceParseResult parse_w3c_trace_context(
    std::string_view traceparent,
    std::string_view tracestate,
    uint16_t max_tracestate_len) noexcept;

std::string format_traceparent(const TraceContext& context);
std::string format_tracestate(const TraceContext& context);

} // namespace hpactor::tracing
```

Parsing rules:

- `traceparent` format is `version-traceid-spanid-flags`.
- Version `00` is supported.
- Trace ID must be 32 lowercase or uppercase hex chars and not all zeros.
- Span ID must be 16 lowercase or uppercase hex chars and not all zeros.
- Flags must be two hex chars; only bit 0 is interpreted.
- Unknown future versions are rejected in v1 rather than partially interpreted.
- Missing `traceparent` is not an error. It means no inbound context.

---

## Message Envelope Changes

### TypedMessage

Add trace context sidecar and preserve it across moves:

```cpp
class TypedMessage {
public:
    bool has_trace_context() const noexcept { return has_trace_context_; }

    const TraceContext& trace_context() const noexcept {
        return trace_context_;
    }

    void set_trace_context(const TraceContext& ctx) noexcept {
        trace_context_ = ctx;
        has_trace_context_ = ctx.valid();
    }

    void clear_trace_context() noexcept {
        trace_context_.clear();
        has_trace_context_ = false;
    }

private:
    TraceContext trace_context_{};
    bool has_trace_context_{false};
};
```

Move constructor and move assignment must copy `trace_context_` and
`has_trace_context_` from the source message. `mpsc_next` stays intentionally
excluded from moves, as it is today.

Expected move behavior:

```text
TypedMessage a has trace T
TypedMessage b(std::move(a))
  -> b.has_trace_context() == true
  -> b.trace_context() == T
```

The moved-from message does not need to clear its trace context because it is no
longer used after move.

### Local Delivery

`ActorSystem::deliver_local()` should not create, replace, or clear trace
context. It only transfers ownership into the mailbox. This keeps local delivery
as a single sink for all local actor messages and avoids hidden root creation in
internal paths such as `enqueue_completion()`.

### Current Trace in ActorContext

Add current trace state to `ActorContext`:

```cpp
class ActorContext {
public:
    bool has_current_trace_context() const noexcept;
    const TraceContext& current_trace_context() const noexcept;

    class TraceScope {
    public:
        TraceScope(ActorContext* ctx, const TraceContext& next) noexcept;
        ~TraceScope();
        TraceScope(const TraceScope&) = delete;
        TraceScope& operator=(const TraceScope&) = delete;
        TraceScope(TraceScope&&) = delete;
        TraceScope& operator=(TraceScope&&) = delete;

    private:
        ActorContext* ctx_{nullptr};
        TraceContext previous_{};
        bool previous_valid_{false};
    };

private:
    void set_current_trace_context(const TraceContext& context) noexcept;
    void clear_current_trace_context() noexcept;

    TraceContext current_trace_context_{};
    bool has_current_trace_context_{false};
};
```

`TraceScope` is a small RAII guard used by receive handling. HPActor is built
without exceptions, but RAII still protects early returns in system-message and
CLI dispatch paths.

---

## Trace Manager

### Ownership

`ActorSystem` owns tracing when runtime config enables it:

```cpp
namespace hpactor {

struct Config {
    // existing fields...
    tracing::TraceConfig tracing;
};

class ActorSystem {
public:
    tracing::TraceManager* trace_manager() noexcept;
    const tracing::TraceManager* trace_manager() const noexcept;
    void apply_tracing_config(const tracing::TraceConfig& config);

private:
    tracing::TraceConfig tracing_config_;
    std::unique_ptr<tracing::TraceManager> trace_manager_;
};

} // namespace hpactor
```

`apply_tracing_config()` is required because `load_topology()` applies TOML
settings after `ActorSystem` construction. It should:

1. Store the config in `tracing_config_`.
2. If tracing is disabled, stop and reset `trace_manager_`.
3. If tracing is enabled and no manager exists, create and start it.
4. If tracing is enabled and a manager exists, update sampler/exporter settings
   that are safe to update live.

The first implementation may support full reconfiguration by stopping and
recreating the manager, as long as it happens before topology actors are spawned
from TOML.

### TraceConfig

```cpp
namespace hpactor::tracing {

enum class TraceExporterKind : uint8_t {
    kNoop,
    kMemory,
    kJsonFile,
    kOtlpHttp,
};

enum class SamplerKind : uint8_t {
    kAlwaysOff,
    kAlwaysOn,
    kTraceIdRatio,
    kParentBasedTraceIdRatio,
};

struct TraceConfig {
    bool enabled{false};
    bool propagate_unsampled{true};
    uint32_t ring_buffer_capacity{65536};
    std::string service_name{"hpactor"};
    SamplerKind sampler{SamplerKind::kParentBasedTraceIdRatio};
    double sample_ratio{0.01};
    TraceExporterKind exporter{TraceExporterKind::kOtlpHttp};
    std::string otlp_endpoint{"http://127.0.0.1:4318/v1/traces"};
    std::string json_file_path;
    std::chrono::milliseconds export_interval{500};
    uint32_t max_export_batch_size{512};
    uint16_t max_tracestate_len{256};
    bool record_actor_receive_spans{true};
    bool record_remote_producer_spans{true};
    bool record_local_producer_spans{false};
    bool record_payload_size{true};
    bool create_roots_for_actor_context_sends{false};
    bool create_roots_for_rpc{true};
    bool create_roots_for_http_ingress{true};
};

} // namespace hpactor::tracing
```

Defaults:

- Compile support is on by default.
- Runtime tracing is off by default.
- Parent-based ratio sampling is the recommended production default.
- Local producer spans are off by default because actor fan-out can be high.
- Remote producer spans are on by default because remote boundaries are where
  tracing gives the most immediate value.

### Span Types

```cpp
namespace hpactor::tracing {

enum class SpanKind : uint8_t {
    kInternal,
    kServer,
    kClient,
    kProducer,
    kConsumer,
};

enum class SpanStatus : uint8_t {
    kUnset,
    kOk,
    kError,
};

struct SpanStart {
    std::string_view name;
    SpanKind kind{SpanKind::kInternal};
    TraceContext parent{};
    bool has_parent{false};
    ActorId actor_id{};
    ActorId sender_actor_id{};
    TypeTag type_tag{TypeTag::Invalid};
    MessageId message_id{};
    uint32_t payload_size{0};
};

struct SpanHandle {
    TraceContext context{};
    uint64_t start_ns{0};
    SpanKind kind{SpanKind::kInternal};
    ActorId actor_id{};
    ActorId sender_actor_id{};
    TypeTag type_tag{TypeTag::Invalid};
    MessageId message_id{};
    uint32_t payload_size{0};
    bool recording{false};
};

struct SpanRecord {
    TraceId trace_id;
    SpanId span_id;
    SpanId parent_span_id;
    ActorId actor_id;
    ActorId sender_actor_id;
    uint32_t type_tag{0};
    uint64_t message_id{0};
    uint64_t start_ns{0};
    uint64_t end_ns{0};
    uint32_t payload_size{0};
    SpanKind kind{SpanKind::kInternal};
    SpanStatus status{SpanStatus::kUnset};
    uint16_t attribute_mask{0};
};

} // namespace hpactor::tracing
```

The hot path span record is numeric and fixed-size. Exporters resolve optional
strings such as actor type names, endpoint text, and route names outside actor
execution.

### TraceManager Facade

```cpp
class TraceManager {
public:
    explicit TraceManager(TraceConfig config, ActorSystem* system);
    ~TraceManager();

    void start();
    void stop();

    bool enabled() const noexcept;
    const TraceConfig& config() const noexcept;

    TraceContext create_root_context(std::string_view operation);
    TraceContext child_context(const TraceContext& parent);

    SpanHandle start_span(const SpanStart& start);
    void finish_span(SpanHandle& span, SpanStatus status) noexcept;

    void inject_message_context(TypedMessage& msg,
                                const ActorContext* ctx,
                                bool allow_root);

    uint64_t spans_dropped() const noexcept;
};
```

Rules:

- `start_span()` returns `recording=false` for disabled tracing, invalid parent
  context with no root policy, or unsampled decisions.
- Even when `recording=false`, `start_span()` can return a valid child context
  for propagation if `propagate_unsampled=true`.
- `finish_span()` is a no-op when `recording=false`.
- `inject_message_context()` preserves explicit message trace context and only
  fills an empty sidecar.

### Ring Buffer

Use the existing `metrics::MpscRingBuffer<T>`:

```cpp
using TraceRingBuffer = metrics::MpscRingBuffer<SpanRecord>;
```

The first version can instantiate the compile-time default capacity. If runtime
capacity must be honored exactly, add a runtime-capacity ring buffer variant or
template specializations for common power-of-two capacities. The implementation
plan should choose one route and keep the API as `TraceManager` ownership, not
as a public template detail.

Overflow behavior:

- `try_push(record)` failure increments `spans_dropped_`.
- Actor execution continues without blocking.
- Metrics expose the dropped count.

---

## Sampling

### Sampler Interface

```cpp
namespace hpactor::tracing {

struct SamplingParameters {
    TraceId trace_id;
    bool has_remote_parent{false};
    bool parent_sampled{false};
};

struct SamplingDecision {
    bool sampled{false};
};

class Sampler {
public:
    virtual ~Sampler() = default;
    virtual SamplingDecision should_sample(
        const SamplingParameters& params) const noexcept = 0;
};

} // namespace hpactor::tracing
```

Built-in samplers:

- `AlwaysOffSampler`: always returns unsampled.
- `AlwaysOnSampler`: always returns sampled.
- `TraceIdRatioSampler`: hashes the trace ID and samples if the value is below
  `sample_ratio`.
- `ParentBasedSampler`: if there is a remote or local parent, reuse its sampled
  flag; otherwise delegate to `TraceIdRatioSampler`.

`ParentBasedTraceIdRatio` is the default because it respects upstream sampling
decisions while still creating roots for HPActor-owned ingress.

### ID Generation

`TraceIdGenerator` must be thread-safe and avoid all-zero IDs:

```cpp
class TraceIdGenerator {
public:
    explicit TraceIdGenerator(EndPoint endpoint);

    TraceId next_trace_id() noexcept;
    SpanId next_span_id() noexcept;

private:
    std::atomic<uint64_t> counter_{1};
    uint64_t seed_hi_{0};
    uint64_t seed_lo_{0};
};
```

Implementation guidance:

- Seed with steady clock, process-local address entropy, endpoint hash, and
  `std::random_device` when available.
- Mix `counter_` through a SplitMix64-style function for each 64-bit half.
- If generated ID is all zeros, increment and retry once.
- Span IDs can use the same generator with a different seed lane.

This is sufficient for opaque operational IDs without adding a crypto dependency
to the actor hot path.

---

## Actor Receive Span Lifecycle

`EventBasedActor::receive()` is the receive span boundary. The span should start
after `ctx` is obtained and before any system-message early return.

Pseudo-flow:

```cpp
void EventBasedActor::receive(TypedMessage& msg) {
    auto* ctx = context();
    if (ctx != nullptr) {
        ctx->set_current_sender(msg.sender_address());
    }

    tracing::SpanHandle span;
    std::optional<ActorContext::TraceScope> scope;

    if (auto* tm = system().trace_manager();
        tm != nullptr && tm->enabled() && tm->config().record_actor_receive_spans) {
        tracing::SpanStart start;
        start.name = "hpactor.actor.receive";
        start.kind = tracing::SpanKind::kConsumer;
        start.has_parent = msg.has_trace_context();
        start.parent = msg.trace_context();
        start.actor_id = id();
        start.sender_actor_id = msg.sender_address().id;
        start.type_tag = msg.type_id();
        start.payload_size = static_cast<uint32_t>(msg.payload().size());

        span = tm->start_span(start);
        if (ctx != nullptr && span.context.valid()) {
            scope.emplace(ctx, span.context);
        }
    }

    // existing system, CLI, proto, behavior dispatch

    if (auto* tm = system().trace_manager(); tm != nullptr) {
        tm->finish_span(span, tracing::SpanStatus::kOk);
    }
}
```

The concrete implementation should avoid `std::optional<TraceScope>` if the
compiler flags complain about move/copy constraints; a small `TraceScopeGuard`
with explicit `activate()`/`reset()` is also acceptable.

Early returns:

- Link, unlink, monitor, demonitor, CLI parse failure, kill request, and proto
  dispatch return paths must finish the span before returning.
- To avoid repeating `finish_span()` at every return, use a no-throw local guard:

```cpp
class ReceiveSpanGuard {
public:
    ReceiveSpanGuard(tracing::TraceManager* manager,
                     tracing::SpanHandle* handle) noexcept;
    ~ReceiveSpanGuard();
    void set_status(tracing::SpanStatus status) noexcept;

private:
    tracing::TraceManager* manager_;
    tracing::SpanHandle* handle_;
    tracing::SpanStatus status_{tracing::SpanStatus::kOk};
};
```

This guard is local to `event_based_actor.cpp` unless it proves useful elsewhere.

---

## Send, Reply, Schedule, RPC, and HTTP Propagation

### ActorContext::send

Before resolving and sending:

1. Stamp sender address as today.
2. If `msg.has_trace_context()`, leave it unchanged.
3. Otherwise ask `TraceManager::inject_message_context(msg, this, allow_root)`.
4. For local targets, deliver as today.
5. For remote targets, `ActorProxy::send()` serializes the sidecar.

`allow_root` is `config.create_roots_for_actor_context_sends`. Default false,
because arbitrary actor sends are often background work and should not create
new traces unless the application has opted in.

### ActorContext::reply

`reply(TypedMessage msg)` calls `send(current_sender_, std::move(msg))`. Because
the current receive span is installed in `ActorContext`, the reply inherits the
receive span context unless the reply message already has an explicit context.

### ActorContext::schedule

The current implementation is a stub. When scheduling is implemented, it should
capture trace context at schedule time:

```text
Actor A receive span active
  context()->schedule(10ms, msg)
    -> msg receives A's active trace context now
timer fires later
  -> deliver_local(target, msg) keeps captured context
```

### ActorContext::rpc

Add an overload or internal path that passes active context to `RpcChannel`:

```cpp
RpcFuture<StreamBuffer>
RpcChannel::call_raw(const ActorAddress& target,
                     const StreamBuffer& encoded_request,
                     std::chrono::milliseconds timeout_ms,
                     const TraceContext* parent_context);
```

`PendingCall` stores:

```cpp
bool has_trace_context{false};
TraceContext trace_context{};
tracing::SpanHandle client_span{};
```

Request send rules:

- First attempt uses client span context when sampled or propagated.
- Retries reuse the same trace ID and client span ID for the logical RPC call,
  while setting `WireFrame::RpcIdempotent` as today.
- A later enhancement can record retry attempts as span events or linked child
  spans. The first version should at least set `rpc.retry_count` on finish.

Response rules:

- `ConnectionPool::on_frame_received()` currently passes only message ID and
  payload to `RpcResponseHandler`.
- Update `RpcResponseHandler` to pass either the full `WireFrame` or a small
  `RpcResponseFrame` containing message ID, payload, and optional trace context.
- `RpcChannel::on_response()` finishes the pending client span before fulfilling
  the promise.

### ActorContext::http_request

Before calling `HttpClient::request()`:

1. Read the current trace context from `ActorContext`.
2. If no context exists and `create_roots_for_rpc` or a dedicated
   `create_roots_for_http_egress` flag is enabled, create a root client span.
3. Inject `traceparent` and `tracestate` headers if absent.
4. Finish the client span when `HttpClient` resolves the future, or finish it
   immediately around the blocking request path used by the current client.

Header names in `HttpHeader` should be lowercase to match current
`HttpRequest::header()` behavior.

---

## Wire Protocol

### Protobuf Schema

Update `protos/hpactor/frame.proto`:

```proto
message PbTraceContext {
  bytes trace_id = 1;      // exactly 16 bytes
  bytes span_id = 2;       // exactly 8 bytes
  uint32 flags = 3;        // W3C trace flags, bit 0 sampled
  string tracestate = 4;   // length <= TraceConfig.max_tracestate_len
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

No framing header change is required. `WireFrame::encode()` and
`WireFrame::decode()` already serialize the protobuf message as the payload
after the `HPAC` header.

### Conversion Helpers

Add helpers in `include/hpactor/net/frame.hpp` and
`src/net/frame_protobuf.cpp`:

```cpp
void to_proto(::hpactor::net::PbTraceContext* pb,
              const TraceContext& context);

result<TraceContext>
trace_context_from_proto(const ::hpactor::net::PbTraceContext& pb,
                         uint16_t max_tracestate_len);
```

`trace_context_from_proto()` validation:

- Missing field is handled by caller through `frame.pb_frame.has_trace_context()`.
- `trace_id().size() != 16` returns an invalid-argument error.
- `span_id().size() != 8` returns an invalid-argument error.
- All-zero trace or span IDs return an invalid-argument error.
- Over-limit tracestate returns an invalid-argument error.

Remote receive should drop only invalid trace sidecars and still deliver the
message.

### ActorProxy::send

After setting payload and type tag:

```cpp
if (msg.has_trace_context()) {
    net::to_proto(frame.pb_frame.mutable_trace_context(),
                  msg.trace_context());
}
```

If producer spans for remote sends are enabled, `ActorProxy::send()` can record
the producer span through `ActorSystem` if the proxy was constructed with a
system pointer. The first vertical slice can record remote producer spans in
`ActorContext::send()` instead because it already knows the active context.

### ActorSystem::deliver_remote

After creating `TypedMessage` and setting sender:

```cpp
if (frame.pb_frame.has_trace_context()) {
    auto parsed = net::trace_context_from_proto(
        frame.pb_frame.trace_context(),
        tracing_config_.max_tracestate_len);
    if (parsed.has_value()) {
        msg.set_trace_context(parsed.value());
    } else {
        // increment malformed trace metric when metrics/tracing is enabled
    }
}
```

Then call `deliver_local()` as today.

---

## Remote Spawn Propagation

### Caller Side

`ActorSystem::spawn_remote_async()` is normally called outside an actor context,
so it should create a root span when tracing is enabled and
`create_roots_for_rpc=true`.

Add:

```cpp
tracing::SpanHandle spawn_span;
if (trace_manager_) {
    tracing::SpanStart start;
    start.name = "hpactor.spawn_remote";
    start.kind = tracing::SpanKind::kClient;
    start.actor_id = system_actor_.id();
    spawn_span = trace_manager_->start_span(start);
    if (spawn_span.context.valid()) {
        net::to_proto(frame.pb_frame.mutable_trace_context(),
                      spawn_span.context);
    }
}
```

`pending_spawns_` should track the span handle by message ID so the caller side
can finish the span when `SpawnResponse` arrives or timeout occurs.

### Receiver Side

`SpawnReceiver::handle_spawn_request()` receives the original `WireFrame`. It
should:

1. Extract trace context from the request frame.
2. Start a server/consumer span named `hpactor.spawn_receive`.
3. Spawn the actor.
4. Build response frame.
5. Put the active spawn handling context into the response frame if valid,
   falling back to the request context.
6. Finish the receiver span with ok or error status.

The existing local unit test path calls `handle_spawn_request(req,
net::WireFrame{})`; that path simply has no inbound trace context.

---

## HTTP Propagation

### Ingress

`HTTPGatewayActor::on_request()` should extract W3C context before route
matching:

```cpp
auto traceparent = req.header("traceparent").value_or("");
auto tracestate = req.header("tracestate").value_or("");
auto parsed = tracing::parse_w3c_trace_context(
    traceparent, tracestate,
    system().trace_manager()->config().max_tracestate_len);
```

Rules:

- Valid inbound context becomes the HTTP server span parent.
- Missing inbound context creates a root only when
  `create_roots_for_http_ingress=true`.
- Malformed inbound context increments a malformed counter and creates a new
  root only when root creation is enabled.
- Route miss and invalid target should finish the HTTP server span before
  sending the error response.

`HTTPGatewayActor` currently wraps actor payloads with an 8-byte request ID:

```text
correlated payload = request_id || original actor payload
```

The implementation must copy trace context from the route-built message to the
correlated message:

```cpp
TypedMessage correlated_msg(msg.type_id(), correlated);
correlated_msg.set_sender_address(reply_adapter_.address());
if (msg.has_trace_context()) {
    correlated_msg.set_trace_context(msg.trace_context());
}
```

The route builder may return a message with an explicit context. If it does not,
`HTTPGatewayActor` applies the HTTP server span context.

### PendingReply

Store the HTTP server span and trace context in `PendingReply`:

```cpp
struct PendingReply {
    uint64_t request_id;
    HTTPConnection* conn;
    std::chrono::steady_clock::time_point enqueued_at;
    bool has_trace_context{false};
    TraceContext trace_context{};
    tracing::SpanHandle server_span{};
};
```

`on_reply()` finishes the server span with `kOk`. `on_timeout()` finishes it
with `kError`. `on_error()` starts and finishes a short server span only when a
request could be parsed enough to extract headers; otherwise it records only a
metric.

HTTP responses should include `traceparent` when a valid trace context exists:

```text
traceparent: 00-<trace_id>-<span_id>-<flags>
```

This is useful for clients and logs even though W3C response propagation is not
required for trace continuity.

### Egress

`ActorContext::http_request()` injects active context before delegating:

```cpp
headers = tracing::inject_w3c_headers(headers, active_context);
return system->http_client().request(method, url, std::move(headers), body);
```

If a caller already supplied `traceparent`, do not overwrite it. That allows
advanced callers to intentionally set a context for cross-system bridging.

---

## Export Pipeline

### Exporter Interface

```cpp
namespace hpactor::tracing {

class SpanExporter {
public:
    virtual ~SpanExporter() = default;

    virtual result<void>
    export_batch(std::span<const SpanRecord> batch) noexcept = 0;

    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

} // namespace hpactor::tracing
```

### Batch Processor

`TraceManager` owns one drain thread when an exporter other than `noop` is
configured:

```text
TraceManager::start()
  -> start drain thread

drain loop:
  wait export_interval
  drain ring buffer into vector<SpanRecord>
  split into max_export_batch_size chunks
  exporter.export_batch(chunk)
  update tracing metrics

TraceManager::stop()
  -> set running false
  -> drain remaining spans once
  -> exporter.shutdown()
  -> join thread
```

Exporter failure:

- The exporter returns `result<void>` with an error.
- TraceManager increments export failure metrics.
- The failed batch is not retried on the actor hot path.
- A bounded retry queue can be added inside `OtlpHttpExporter`; if full, it
  drops oldest batches and increments dropped-batch metrics.

### MemoryExporter

The memory exporter is required for deterministic tests:

```cpp
class MemoryExporter final : public SpanExporter {
public:
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;
    std::vector<SpanRecord> snapshot() const;
    void clear();
};
```

Tests should be able to call `trace_manager()->force_flush()` to drain without
sleeping.

### JsonFileExporter

JSON lines exporter writes one span per line for local debugging:

```json
{"trace_id":"...","span_id":"...","parent_span_id":"...","kind":"consumer","actor_id":42,"type_tag":4096,"start_ns":123,"end_ns":456,"status":"ok"}
```

The first implementation can support stdout when `json_file_path` is empty.

### OtlpHttpExporter

OTLP export should map `SpanRecord` to OpenTelemetry protobuf `ResourceSpans`.
Implementation options:

1. Add a minimal `protos/opentelemetry/trace/v1/trace.proto` and
   `protos/opentelemetry/resource/v1/resource.proto` subset.
2. Build JSON OTLP payloads for `/v1/traces` when the collector accepts JSON.

Recommended first production exporter: OTLP/HTTP protobuf if adding the proto
subset is acceptable. JSON exporter remains the fallback when avoiding more
generated files is preferable for the first integration.

---

## Metrics and Logging Integration

Tracing emits operational metrics through the existing metrics subsystem:

| Metric | Type | Labels | Source |
|--------|------|--------|--------|
| `hpactor_tracing_spans_started_total` | Counter | `kind` | `TraceManager::start_span()` sampled path |
| `hpactor_tracing_spans_finished_total` | Counter | `kind`, `status` | `TraceManager::finish_span()` |
| `hpactor_tracing_spans_dropped_total` | Counter | none | ring buffer full |
| `hpactor_tracing_context_malformed_total` | Counter | `source` | HTTP or protobuf parse failure |
| `hpactor_tracing_export_batches_total` | Counter | `exporter`, `status` | exporter drain |
| `hpactor_tracing_export_latency_seconds` | Histogram | `exporter` | exporter drain |

The implementation can initially store these as counters on `TraceManager` and
bridge them into the metrics aggregator in a follow-up patch. The public metric
names should be reserved from the start.

Logging integration rule:

- Logs do not start spans.
- A log event copies active trace and span IDs from `ActorContext` when present.
- If a log is emitted while processing a `TypedMessage` before a receive span is
  active, it may copy the message sidecar instead.

---

## Configuration

### CMake

Add:

```cmake
option(ENABLE_ACTOR_TRACING "Enable distributed tracing subsystem" ON)
```

Generate:

```cpp
#define HPACTOR_ENABLE_ACTOR_TRACING 1
```

through `include/hpactor/hpactor_config.hpp.in`, following the CLI and
proactor pattern.

When `ENABLE_ACTOR_TRACING=OFF`:

- Tracing source files are not added to `hpactor_lib`.
- `ActorSystem::trace_manager()` returns `nullptr`.
- `TypedMessage` may still carry `TraceContext` only if the team wants wire
  compatibility with tracing-enabled builds. The lower-overhead option is to
  keep the sidecar compiled in because it is a small fixed-size struct and keeps
  ABI behavior simpler across build options.

### Config Struct

Add `tracing::TraceConfig tracing;` to `hpactor::Config`.

`ActorSystem` constructor calls `apply_tracing_config(config.tracing)` after the
scheduler starts and before network/HTTP actors are spawned.

### TOML

Add `[system.tracing]`:

```toml
[system.tracing]
enabled = true
service_name = "checkout-actors"
sampler = "parent_based_trace_id_ratio"
sample_ratio = 0.05
propagate_unsampled = true
ring_buffer_capacity = 65536
exporter = "json_file"
json_file_path = "trace-spans.jsonl"
otlp_endpoint = "http://127.0.0.1:4318/v1/traces"
export_interval_ms = 500
max_export_batch_size = 512
max_tracestate_len = 256
record_actor_receive_spans = true
record_remote_producer_spans = true
record_local_producer_spans = false
record_payload_size = true
create_roots_for_actor_context_sends = false
create_roots_for_rpc = true
create_roots_for_http_ingress = true
```

Parser behavior:

- Unknown sampler string returns a parse error.
- Unknown exporter string returns a parse error.
- `sample_ratio` must be in `[0.0, 1.0]`.
- `ring_buffer_capacity` must be a power of two if the implementation supports
  runtime capacity.
- `max_tracestate_len` must be `<= 256`.

### TopologyModel

Add fields to `config::SystemDef` or embed `tracing::TraceConfig` directly:

```cpp
tracing::TraceConfig tracing;
```

Embedding is cleaner, but it introduces a dependency from config model to the
tracing header. That dependency is acceptable because config already depends on
CLI config.

### Binary Topology

`BinarySystemDef` must include tracing fields. Because it is a fixed binary
format, bump the binary format version when adding fields.

Recommended fields:

```cpp
uint32_t tracing_enabled;
uint32_t tracing_propagate_unsampled;
uint32_t tracing_ring_buffer_capacity;
uint32_t tracing_sampler;          // SamplerKind
uint32_t tracing_exporter;         // TraceExporterKind
double   tracing_sample_ratio;
uint32_t tracing_export_interval_ms;
uint32_t tracing_max_export_batch_size;
uint16_t tracing_max_tracestate_len;
uint32_t tracing_flags;            // bitset for booleans
uint32_t tracing_service_name_offset;
uint32_t tracing_otlp_endpoint_offset;
uint32_t tracing_json_file_path_offset;
```

`BinaryLoader` must default missing tracing fields to `TraceConfig{}` when
loading an older binary version.

---

## Compatibility and Failure Handling

| Case | Required Behavior |
|------|-------------------|
| Tracing disabled | Messages deliver as they do today. No spans exported. Trace sidecar is preserved only if explicitly set. |
| Missing trace context | No root is created except at configured root boundaries. |
| Malformed HTTP traceparent | Drop inbound context, count malformed context, optionally create new root. |
| Malformed protobuf trace context | Drop trace sidecar only; deliver message. |
| Mixed-version cluster | New protobuf field is ignored by old nodes; message delivery still works. |
| Ring buffer full | Drop span record and increment dropped span counter. |
| Exporter failure | Return error to TraceManager; do not block actor path. |
| Actor receive early return | Receive span is finished by guard. |
| Actor receives unsampled context | Context propagates if `propagate_unsampled=true`; no span record is emitted. |
| HTTP route miss | Finish HTTP server span with error status and return 404. |
| RPC timeout | Finish client span with error status and fulfill promise with timeout error. |

---

## Test Strategy

### Unit Tests

Create `tests/tracing/test_trace_context.cpp`:

- default `TraceContext` is invalid.
- all-zero trace ID is invalid.
- all-zero span ID is invalid.
- sampled flag set/clear works.
- move construction of `TypedMessage` preserves trace context.
- move assignment of `TypedMessage` preserves trace context.

Create `tests/tracing/test_w3c_trace_context.cpp`:

- valid `traceparent` parses into exact bytes.
- uppercase hex parses and formats back as lowercase.
- sampled flag `01` is detected.
- flags `00` is unsampled.
- all-zero trace ID is rejected.
- all-zero span ID is rejected.
- wrong field lengths are rejected.
- over-limit `tracestate` is rejected.
- missing `traceparent` returns `kMissing`.

Create `tests/tracing/test_sampler.cpp`:

- always on samples.
- always off does not sample.
- ratio 0.0 never samples.
- ratio 1.0 always samples.
- parent-based sampler respects sampled parent.
- parent-based sampler respects unsampled parent.
- fixed trace ID gives deterministic ratio decision.

Create `tests/tracing/test_trace_manager.cpp`:

- disabled manager returns non-recording spans.
- enabled memory exporter records a finished span after force flush.
- unsampled context propagates but does not export a span.
- ring buffer overflow increments dropped count.

### Integration Tests

Create `tests/tracing/test_trace_message_propagation.cpp`:

- local send from actor A to actor B preserves trace ID.
- reply from B to A preserves trace ID and uses B receive span as parent.
- explicit message trace context is not overwritten by `ActorContext::send()`.
- system `DownMsg` sent from `on_exit()` carries active context when one exists.

Create `tests/tracing/test_trace_wire_propagation.cpp`:

- `ActorProxy::send()` writes `PbTraceContext` to `ActorMsgFrame`.
- `ActorSystem::deliver_remote()` reads `PbTraceContext` into `TypedMessage`.
- old frame with no `trace_context` delivers normally.
- invalid trace ID size in protobuf drops only the sidecar.

Create `tests/tracing/test_trace_rpc_spawn.cpp`:

- `RpcChannel::call_raw()` writes request trace context.
- RPC response finishes client span.
- RPC timeout finishes client span with error.
- `spawn_remote_async()` writes request trace context.
- `SpawnReceiver` response includes trace context.

Create `tests/tracing/test_trace_http_propagation.cpp`:

- HTTP ingress extracts valid `traceparent` and delivers actor message with same
  trace ID.
- HTTP ingress creates a root when header is missing and root creation is
  enabled.
- HTTP ingress rejects malformed traceparent and increments malformed counter.
- HTTP gateway request-id wrapping preserves message trace context.
- HTTP egress injects `traceparent` into outbound headers.
- Existing caller-supplied `traceparent` header is not overwritten.

Create `tests/tracing/test_trace_config.cpp`:

- TOML parser reads `[system.tracing]` defaults.
- TOML parser rejects invalid sampler.
- TOML parser rejects invalid exporter.
- Binary topology round trip preserves tracing fields.
- Loading an older binary topology defaults tracing disabled.

### Verification Commands

Standard build and test:

```bash
cmake -S . -B build -GNinja
ninja -C build
ctest --output-on-failure
```

Focused tests:

```bash
./build/tests/test_trace_context
./build/tests/test_w3c_trace_context
./build/tests/test_sampler
./build/tests/test_trace_manager
./build/tests/test_trace_message_propagation
./build/tests/test_trace_wire_propagation
./build/tests/test_trace_rpc_spawn
./build/tests/test_trace_http_propagation
./build/tests/test_trace_config
```

Compile-off verification:

```bash
cmake -S . -B build-no-tracing -GNinja -DENABLE_ACTOR_TRACING=OFF
ninja -C build-no-tracing
ctest --test-dir build-no-tracing --output-on-failure
```

---

## Implementation Sequence Guidance

This is not the step-by-step implementation plan, but it defines the safest
order for that plan.

### Phase 1: Trace Types and Message Sidecar

Deliverables:

- Expanded `TraceId`, `SpanId`, `TraceFlags`, and `TraceContext`.
- W3C parse/format helpers.
- `TypedMessage` trace context sidecar and move semantics.
- Unit tests for context, parser, and message moves.

Reasoning: this phase is purely local and does not touch actor scheduling,
transport, or exporters.

### Phase 2: TraceManager With Memory Exporter

Deliverables:

- `TraceConfig`, samplers, ID generator, `SpanRecord`, `TraceManager`.
- Memory exporter and force flush for tests.
- ActorSystem ownership and runtime config.
- Unit tests for sampling, manager, overflow, and force flush.

Reasoning: this creates the data path before instrumentation points start using
it.

### Phase 3: Local Actor Propagation

Deliverables:

- `ActorContext` current trace scope.
- `ActorContext::send()` and `reply()` propagation.
- `EventBasedActor::receive()` consumer spans with early-return guard.
- Local actor propagation integration tests.

Reasoning: local propagation is the minimal useful tracing feature and catches
most ownership mistakes before protobuf changes.

### Phase 4: Wire, RPC, and Spawn Propagation

Deliverables:

- `PbTraceContext` in `frame.proto`.
- Frame conversion helpers.
- `ActorProxy::send()` and `ActorSystem::deliver_remote()` propagation.
- RPC pending call trace context and response span finish.
- Remote spawn request/response propagation.
- Wire/RPC/spawn integration tests.

Reasoning: cross-node propagation depends on stable local trace semantics.

### Phase 5: HTTP Ingress and Egress

Deliverables:

- HTTP gateway extraction and response header injection.
- Pending reply trace state.
- Request-id wrapping sidecar preservation.
- ActorContext HTTP egress header injection and client spans.
- HTTP propagation tests.

Reasoning: HTTP spans need both local propagation and parser helpers.

### Phase 6: Config, Metrics, and Production Export

Deliverables:

- TOML and binary topology tracing config.
- CMake compile option and generated config macro.
- Reserved tracing metrics.
- JSON exporter.
- OTLP/HTTP exporter or documented JSON-to-collector bridge.
- Compile-off verification.

Reasoning: production export and config have broader blast radius and should
land after behavior is proven with memory exporter tests.

---

## Definition of Done

The detailed design is implemented when:

- All trace IDs and span IDs use W3C/OpenTelemetry-compatible byte widths.
- `TypedMessage` carries trace context without changing user payloads.
- Local sends, replies, and actor receive spans propagate trace context.
- Remote actor frames serialize and deserialize trace context.
- RPC and remote spawn preserve trace context and finish client spans.
- HTTP ingress extracts `traceparent`; HTTP egress injects `traceparent`.
- Sampled spans export through memory exporter and at least one file or OTLP
  production exporter.
- Tracing config works through both `hpactor::Config` and TOML topology.
- Tracing disabled preserves existing behavior.
- The standard test suite passes with tracing enabled and disabled.
