# ActorSystem Phase 4 Frame and Stream Routing Design

**Date:** 2026-06-28

**Status:** Proposed phase design

**Parent design:**
`docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`

**Prerequisites:** Phase 0 correctness stabilization, Phase 1 runtime ownership
shell, Phase 2 `ActorRuntime`, and Phase 3 `MessagingRuntime` are merged. Their
focused normal, ASan, and TSAN verification passes, including stable messaging
ports and the explicit `FastDeliveryReason::StreamProtocol` seam.

**Scope:** Introduce one `InboundFrameRouter`, normalize inbound wire framing
and typed decode outcomes, route ordinary, batch, RPC, reliable-control,
backpressure, and stream envelopes to their owners, and move stream registry
state and handlers into a bounded `StreamRuntime`. Keep event-loop, connection,
transport, discovery, and network-thread ownership in the Phase 1 shell until
Phase 5.

## 1. Summary

Phase 4 removes protocol demultiplexing and stream state from `ActorSystem`.
Every inbound HPActor wire envelope follows one path:

```text
connection framing
  -> typed WireFrame decode outcome
  -> InboundFrameRouter
       +-- ordinary/batch actor data -> MessagingRuntime
       +-- reliable ACK/NACK --------> MessagingRuntime
       +-- backpressure -------------> MessagingRuntime coordinator
       +-- RPC response -------------> RpcChannel
       +-- stream protocol ----------> StreamRuntime
       +-- invalid/unsupported ------> typed result + bounded observability
```

`StreamRuntime` owns one peer-qualified, bounded stream-session registry,
stream-id allocation, stream actor registration, inbound stream handlers, and
registry snapshots. Its mutex protects only registry structure. It copies the
required route records and releases the mutex before actor spawning, message
delivery, wire output, logging, metrics, or callbacks.

The public `ActorSystem::deliver_remote()` and stream methods remain
source-compatible adapters. `deliver_remote()` becomes one call to the router
and discards the typed result according to the existing `void` contract. No
private stream map, stream counter, or frame-specific handler remains in the
facade/runtime shell.

Phase 4 deliberately does not take ownership of the transport or event loop.
It installs a fixed inbound sink into the existing transport state. Phase 5
moves that wiring and lifecycle into `NetworkRuntime` without changing router
or stream policy.

## 2. Current-State Evidence

Code-graph and source inspection show protocol policy split across three
layers:

1. `WireFrameConnection` reads the eight-byte HPAC header but strips it before
   invoking its frame handler.
2. TLS delivery passes decrypted plaintext to the same handler without the
   same normalization guarantee.
3. `ConnectionPool::on_frame_received()` calls `WireFrame::decode()`, which
   expects the HPAC header, and separately handles RPC/spawn responses before
   forwarding all other envelopes.
4. `ActorSystem::deliver_remote()` handles five stream oneof variants, then
   assumes every remaining envelope is `data_frame`, interprets reliable
   flags, detects backpressure by TypeTag, decodes ordinary data, and delivers
   it locally.
5. `WireFrame::PayloadType` also exposes dedicated ACK, NACK, and Batch oneof
   variants, but `deliver_remote()` does not dispatch them.
6. `deliver_remote_batch()` is declared but no production definition or batch
   admission path is indexed. Existing tests cover batch encode/decode, not
   receiver delivery.
7. Stream sender/receiver maps are plain `unordered_map`s accessed from public
   stream methods and network callbacks without a mutex.
8. Stream close/error handlers construct user-facing tags while the protocol
   actors expect internal wire tags. ACK handling copies the in-memory C++
   protobuf object representation rather than serialized protobuf bytes.
9. Unknown stream ids and malformed control payloads are silently dropped.
10. Stream CLI commands currently render an empty future table, and existing
    stream metric event types are not connected to the registry/router.

These are correctness and operational gaps, not merely aesthetic reasons to
move methods into new files.

## 3. Important Correctness Findings

Each finding requires a characterization or regression test before the
affected path moves.

### 3.1 Plain and TLS callbacks do not share one framing contract

`WireFrame::encode()` produces `HPAC + network-order length + protobuf`.
`WireFrame::decode()` expects that shape. The plain connection currently strips
the header before `ConnectionPool` calls the decoder, while the TLS path can
deliver a different plaintext shape. Actual ingress can therefore collapse to
an empty frame despite encode/decode unit tests passing.

Required contract:

- every transport frame handler receives exactly one canonical encoded HPAC
  frame, including its eight-byte header;
- connection code validates the header and bounds the advertised length before
  reserving payload memory;
- plain and TLS paths produce the same callback bytes;
- an encoded frame is decoded exactly once; and
- the contract is proven through socket-level plain/TLS integration tests, not
  only direct `WireFrame::decode()` calls.

### 3.2 Frame length can drive unbounded allocation and recursive resync

The plain connection reads the advertised payload length and reserves that
amount without a configured inbound limit. Invalid magic handling recursively
re-enters `handle_read()`, so a hostile byte stream can create deep recursion.

Required contract:

- reject an advertised payload larger than `max_inbound_frame_bytes` before
  allocation;
- the Phase 4 default is 16 MiB, matching the existing default endpoint
  outbound byte bound;
- header resynchronization is iterative and bounded per event-loop turn;
- oversize/truncated/invalid framing produces a typed decode/framing result;
- connection objects report failures through a dedicated framing-error
  callback to their pool even when no complete frame can be emitted; and
- no blocking scan or unbounded allocation is added to the event loop.

### 3.3 Decode failure is indistinguishable from a valid empty envelope

Short input, bad magic, truncated payload, protobuf parse failure, and a valid
empty/unknown envelope all become `WireFrame{}`. Callers cannot choose a safe
policy or count the real failure.

Required contract: add a strict `WireFrame::try_decode()` API returning one of:

- `HeaderTooShort`;
- `InvalidMagic`;
- `FrameTooLarge`;
- `LengthMismatch`;
- `TrailingBytes`;
- `InvalidProtobuf`; or
- a valid `WireFrame`.

The existing `decode()` remains a source-compatible wrapper returning an empty
frame on error. Production ingress uses `try_decode()` and never infers failure
from a default object.

An empty or future unknown protobuf oneof is a valid decode with
`PayloadType::Unknown`; the router reports `UnsupportedPayload`, not
`InvalidProtobuf`. This distinction preserves forward-compatible diagnosis.

### 3.4 Demultiplexing is split and order-dependent

`ConnectionPool` owns RPC/spawn classification while `ActorSystem` owns the
rest. Any new payload type must be edited in both places, and a flag collision
can be consumed by the wrong layer.

Required contract: when a unified inbound sink is installed,
`ConnectionPool` performs framing/decode only. `InboundFrameRouter` is the sole
valid-envelope classifier. Legacy split handlers remain only as a direct
transport compatibility fallback when no unified sink is installed; unified
and legacy handlers never both receive one frame.

### 3.5 `AckRequested` is currently both a request and an ACK response

Ordinary reliable data sets `AckRequested`, but `deliver_remote()` interprets
that bit first as an inbound ACK and returns without delivering the message.
The same bit is also emitted for accepted ACK responses. The receiver cannot
unambiguously distinguish those meanings.

Required Phase 4 compatibility rule for legacy data-frame control:

| Flags | Meaning |
|---|---|
| `AckRequested` only | ordinary actor data requesting a receiver ACK |
| `AckRequested | AckResponse` | legacy accepted/duplicate ACK response |
| `AckResponse` only | legacy rejected NACK response |

Outgoing legacy ACKs set both bits. Existing nodes check `AckRequested` first
and therefore continue to treat the dual-bit frame as ACK. New nodes can
deliver request-only frames correctly. Dedicated `ack_frame` and `nack_frame`
oneofs are also accepted, but switching all outgoing traffic to them requires a
separate rolling-compatibility decision.

Conflicting RPC/control combinations are rejected as `InvalidFlags` rather
than relying on branch order.

### 3.6 Dedicated ACK/NACK oneofs are not dispatched

`WireFrame::from_ack()` and `from_nack()` produce explicit oneof variants, but
the current facade falls through to `data_frame()` fields.

Required contract: route dedicated ACK and NACK to the typed Phase 3
`MessagingRuntime` handlers. Map protobuf `NackReason` to canonical
`DeliveryStatus` through an explicit table; never `static_cast` unrelated
status enums. Unknown reason values return `InvalidControlPayload`.

Legacy `AckStatus` values are mapped explicitly: accepted and duplicate resolve
as ACK; rejected maps to the current mailbox-rejection status and retains the
retry-after hint. Phase 4 does not broaden reliable resend semantics.

### 3.7 Batch envelopes are encoded but not delivered

Batch send builds a common sender/receiver plus ordered entries, but receiver
delivery is missing. Falling through to `data_frame()` loses the batch.

Required contract:

- validate `entries_size <= max_batch_entries` (default 1024) before iterating;
- process entries in encoded order on the event-loop producer thread;
- preserve per-entry TypeTag, message id, flags, payload, and trace context;
- use the same full `MessagingRuntime` delivery path as ordinary data;
- continue after an individual entry rejection and return aggregate accepted,
  rejected, and invalid counts;
- report `BatchPartiallyDelivered` for mixed results; and
- do not promise atomic all-or-nothing batch admission or cross-lane execution
  order.

The batch frame has no priority/deadline fields, so Phase 4 retains current
default priority and deadline behavior rather than inventing metadata.

### 3.8 Stream registries have a data race

Network callbacks read/erase stream maps while external callers can register,
open, or unregister streams. The maps have no lock.

Required contract: `StreamRuntime` owns one mutex-protected session map. The
mutex protects only keys, registration state, capacity, and snapshot copying.
No actor spawn, stop, delivery, wire send, logging, metric emission, or callback
runs while it is held.

Lookup linearizes under the mutex. A handler that acquires a route before a
concurrent close may complete that one delivery; handlers beginning after the
close erase observe `UnknownStream`. Frames from one connection remain ordered
because its event-loop callback is serialized.

### 3.9 A bare stream id is not a distributed identity

Two peers can choose the same 64-bit stream id. Current maps key only by id, so
one peer can collide with or control another peer's stream. Frame handlers also
cannot verify that the sender address in `StreamOpen` matches the connection
peer.

Required contract:

```cpp
struct StreamKey {
    EndPoint peer;
    uint64_t stream_id;
};
```

All remote stream lookup uses the transport-authenticated peer plus stream id.
`StreamOpen.sender.endpoint` must match that peer when the context is known.
Local compatibility calls use the local endpoint as peer. Stream ids are opaque
per-origin monotonic values; uniqueness is enforced against live keys rather
than by packing truncated actor-id bits.

### 3.10 Stream protocol messages use incorrect representations and tags

The ACK handler copies `sizeof(protobuf_object)` bytes from the object's C++
memory. Close/error handlers send `StreamClosedTag`/`StreamErrorTag`, while
`StreamSenderActor` and `StreamReceiverActor` parse `StreamCloseTag` and
`StreamWireErrorTag`. Those actor handlers can silently ignore the frames.

Required contract:

- construct internal `TypedMessage` values with the protobuf-aware constructor;
- route data as `StreamDataTag`;
- route ACK as `StreamAckTag`;
- route close as `StreamCloseTag`;
- route error as `StreamWireErrorTag`;
- reserve `StreamClosedTag` and `StreamErrorTag` for actor-to-user notification;
  and
- preserve stream error code/description and close reason, rather than replacing
  them with empty payloads.

### 3.11 Duplicate, unknown, and unbounded stream state is silent

Registration overwrites duplicate ids, unknown data/ACK is dropped, and remote
opens can grow maps without a bound.

Required contract:

- `max_active_streams` defaults to 4096 and includes opening reservations;
- duplicate `(peer, stream_id)` returns `DuplicateStream` without replacing the
  existing route;
- over-capacity open returns `StreamCapacityExceeded`;
- data/ACK/close/error for a missing key returns `UnknownStream`;
- close/error removes the session exactly once; and
- all outcomes emit bounded metric codes and rate-limited structured logs.

Stream open reserves an `Opening` entry under the lock, releases the lock for
actor spawn, then commits `Active` or removes the reservation. A concurrent
close can mark/remove the reservation; a late successful spawn is stopped
through the narrow actor lifecycle port instead of becoming an orphan.

### 3.12 Trace parsing uses a hard-coded limit

Stream open parses trace state with `256`, while ordinary/RPC paths use other
configuration values. Invalid trace metadata is silently discarded.

Required contract: the router and stream runtime receive the immutable
effective `max_tracestate_len`. Invalid optional trace context does not expose
payload data; it produces a bounded metadata-warning result/metric. Existing
ordinary delivery behavior—deliver without invalid optional trace—is preserved.

### 3.13 Inbound callback lifetime is not explicit

Transport callbacks currently capture `ActorSystem`. Moving them to a raw
router pointer without stop ordering would exchange one hidden lifetime edge
for another.

Required contract:

- the transport stores a fixed `InboundFrameSink` function-pointer/context
  value installed before ingress starts;
- the sink context is stable and outlives transport callbacks;
- router dependencies outlive the router;
- shutdown disables ingress, stops/joins the event loop, and only then destroys
  router, stream, messaging, RPC, and actor components; and
- a disabled router returns `RuntimeStopping` without invoking downstream code.

Phase 5 makes this ordering the responsibility of `NetworkRuntime`; Phase 4
records and verifies it in the shell.

### 3.14 Existing stream API is scaffolding, not a complete remote stream

`StreamHandle::write()` and `close()` currently mutate only handle-local state,
and stream actors use local fast delivery for protocol output. `open_stream()`
looks up a local actor before its nominal remote branch. The code does not prove
end-to-end remote stream session functionality.

Required contract: Phase 4 does not claim or silently invent a complete public
remote streaming API. It makes inbound routing, registry ownership, protocol
message representation, bounds, and observability correct. Completing
`StreamHandle` commands and actor-to-remote wire output requires a separate
stream feature design after this extraction. Unsupported public remote open
continues to fail explicitly (`nullopt` through the compatibility API), rather
than reporting a false successful session.

## 4. Goals

1. Make `InboundFrameRouter` the sole classifier of valid HPActor envelopes.
2. Normalize plain/TLS frame-handler bytes and add typed strict decode results.
3. Route ordinary and batch data through Phase 3 `MessagingRuntime`.
4. Route RPC responses directly to `RpcChannel` without facade ownership.
5. Route dedicated and compatible legacy ACK/NACK through typed messaging
   handlers without flag ambiguity.
6. Route decoded backpressure to its messaging owner.
7. Make `StreamRuntime` the sole owner of stream ids, registrations, handlers,
   bounds, and snapshots.
8. Preserve protobuf and TypeTag wire compatibility while correcting tested
   invalid representations/flags.
9. Return typed results for malformed, unsupported, invalid, partial, unknown,
   duplicate, capacity, unavailable-handler, and stopping outcomes.
10. Preserve event-loop non-blocking behavior and actor/mailbox concurrency
    contracts.
11. Preserve public `ActorSystem` remote/stream signatures as forwards.
12. Leave transport/event-loop lifecycle and ownership for Phase 5.

## 5. Non-Goals

- Moving `TcpTransport`, `ConnectionPool`, event loop/thread, discovery,
  location cache, RPC channel, HTTP client, or remote spawn ownership.
- Completing `StreamHandle::write`, remote stream output, stream retransmission,
  or durable stream recovery.
- Changing protobuf field numbers, TypeTag assignments, HPAC magic/header, or
  the frame oneof schema.
- Removing legacy transport handler setters used by direct tests/applications.
- Introducing per-peer quarantine, authentication, or authorization policy;
  Phase 4 supplies typed peer-aware results for later policy.
- Adding atomic batch delivery or changing mailbox lane semantics.
- Redesigning reliable retry/resend or merging trackers.
- Adding a generic handler registry, service locator, DI container, virtual
  router hierarchy, or actor-based stream registry.
- Holding actor state in stream snapshots or reading actor internals from CLI.
- Parsing TOML in the router/stream component.

## 6. Considered Approaches

### 6.1 Move current methods into `ActorSystem::Impl`

This shortens the public source file but keeps split routing, callback capture,
unbounded stream maps, and invalid frame assumptions. Rejected as storage
relocation without subsystem boundaries.

### 6.2 Add a router only after `ConnectionPool` pre-classification

Leave RPC/spawn and decode failures in `ConnectionPool`; route only actor,
control, and stream frames in a new class.

This creates two routers and still requires edits in both for new flags/types.
Malformed framing remains invisible. Rejected.

### 6.3 Make the stream registry an actor

Network callbacks would enqueue every lookup/register/close request to one
registry actor.

This gives serialized ownership but adds asynchronous round trips to every
stream frame, complicates immediate typed dispatch results, and does not remove
the need for safe callback lifetime. Rejected for this refactor.

### 6.4 Unified decode sink, concrete router, and mutex-bounded stream component

Normalize connection framing, pass typed decode events through one fixed sink,
use a concrete router with direct component references, and protect one
peer-qualified stream registry with a narrow mutex.

Selected. It establishes one classification source, explicit failure results,
bounded state, and stable Phase 5 seams without generic runtime lookup or
per-frame handler allocation.

## 7. Target Phase 4 Architecture

```text
WireFrameConnection / TlsConnection
  | normalized canonical encoded HPAC frame
  v
ConnectionPool
  | WireFrame::try_decode + InboundFrameContext(peer, bytes)
  | fixed InboundFrameSink
  v
InboundFrameRouter
  +-- Data ordinary ---------------------> MessagingRuntime::try_deliver
  +-- Data RpcResponse ------------------> RpcChannel::on_response
  +-- Data legacy ACK/NACK --------------> MessagingRuntime typed control
  +-- Ack/Nack oneof --------------------> MessagingRuntime typed control
  +-- Data Backpressure -----------------> MessagingRuntime typed pressure
  +-- Batch -----------------------------> per-entry full messaging delivery
  +-- Stream* ---------------------------> StreamRuntime
  +-- Unknown/invalid -------------------> FrameDispatchResult + observability

StreamRuntime
  | owns bounded map<StreamKey, StreamSession>
  +-- stream actor spawn/stop -----------> narrow ActorRuntime port
  +-- internal protocol actor message ---> MessagingRuntime fast reason
  +-- future stream wire output ---------> narrow transitional network port
  +-- snapshot --------------------------> ActorSystem/CLI compatibility view
```

`ActorSystem::Impl` composes these concrete components. It orchestrates their
startup/stop order but owns no routing policy.

## 8. Frame Decode and Sink Contracts

### 8.1 Strict decode result

Recommended public additive types in the existing frame module:

```cpp
enum class FrameDecodeError : uint8_t {
    None,
    HeaderTooShort,
    InvalidMagic,
    FrameTooLarge,
    LengthMismatch,
    TrailingBytes,
    InvalidProtobuf,
};

struct FrameDecodeLimits {
    uint32_t max_payload_bytes{16U * 1024U * 1024U};
    bool reject_trailing_bytes{true};
};

struct FrameDecodeResult {
    WireFrame frame;
    FrameDecodeError error{FrameDecodeError::None};
    uint32_t declared_payload_bytes{0};

    [[nodiscard]] bool ok() const noexcept;
};
```

`try_decode()` validates exact arithmetic without overflow before constructing
the protobuf input. Compatibility `decode()` calls it and returns an empty
frame on failure.

### 8.2 Inbound context and fixed sink

```cpp
struct InboundFrameContext {
    EndPoint peer;
    uint32_t encoded_bytes{0};
};

struct InboundFrameSink {
    void* context{nullptr};
    FrameDispatchResult (*route)(void*, const InboundFrameContext&,
                                 const WireFrame&) noexcept{nullptr};
    FrameDispatchResult (*decode_failed)(void*, const InboundFrameContext&,
                                         FrameDecodeError) noexcept{nullptr};
};
```

The sink is copied into connection pools and contains no owning pointer,
allocation, or facade capture. It is installed before ingress starts and is not
mutated while callbacks run.

Connection objects retain a lower-level callback pair: one delivers a complete
canonical encoded frame to its pool, and one reports `FrameDecodeError` plus
observed byte count when framing fails before a complete frame exists. The pool
adds its authenticated `remote_endpoint_` and forwards both outcomes through
`InboundFrameSink`. This keeps peer identity out of byte framing while ensuring
oversize and resynchronization failures are observable.

When no unified sink is installed, direct transport users retain the existing
legacy RPC/actor handler behavior. When it is installed, it has exclusive
dispatch precedence.

## 9. `FrameDispatchResult`

The router returns a fixed-size diagnostic value:

```cpp
enum class FrameDispatchCode : uint8_t {
    ActorDelivered,
    ActorRejected,
    BatchDelivered,
    BatchPartiallyDelivered,
    RpcResponseHandled,
    ReliableAckHandled,
    ReliableNackHandled,
    BackpressureHandled,
    StreamHandled,
    DecodeFailed,
    UnsupportedPayload,
    InvalidFlags,
    InvalidAddress,
    InvalidControlPayload,
    InvalidTraceContext,
    UnknownStream,
    DuplicateStream,
    StreamCapacityExceeded,
    HandlerUnavailable,
    RuntimeStopping,
};

struct FrameDispatchResult {
    FrameDispatchCode code;
    net::WireFrame::PayloadType payload_type;
    uint32_t detail_code{0};
    uint32_t accepted_count{0};
    uint32_t rejected_count{0};
    uint32_t invalid_count{0};
};
```

Exact enum placement may avoid a circular include, but meanings and fixed-size
fields are required. The result contains no payload, dynamic string, actor
pointer, or unbounded error detail.

Phase 4 records the result and drops according to current behavior. Phase 5
maps it to peer close/quarantine policy. The router does not close connections.

## 10. `InboundFrameRouter` Contract

Recommended private interface:

```cpp
class InboundFrameRouter final {
  public:
    struct Config {
        uint32_t max_batch_entries{1024};
        uint16_t max_tracestate_len{256};
    };

    struct Dependencies {
        runtime::MessagingRuntime& messaging;
        StreamRuntime& streams;
        RpcChannel& rpc;
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics{nullptr};
    };

    InboundFrameRouter(Dependencies dependencies, Config config) noexcept;

    FrameDispatchResult route(const InboundFrameContext& context,
                              const net::WireFrame& frame) noexcept;
    FrameDispatchResult on_decode_failure(const InboundFrameContext& context,
                                          FrameDecodeError error) noexcept;
    void disable() noexcept;
};
```

The router stores concrete references, immutable limits, a stable optional
metrics pointer, and one atomic accepting flag. It owns no transport, actor,
message queue, stream map, tracker, or RPC pending state.

### 10.1 Classification order

Classification is by protobuf oneof first. Only a `Data` payload reads data
flags or data fields.

For `Data`:

1. validate required receiver/sender structure relevant to the selected path;
2. reject conflicting `RpcResponse` and control-response bits;
3. recognize `RpcResponse`;
4. recognize the dual-bit legacy ACK and response-only legacy NACK;
5. recognize reserved backpressure TypeTag;
6. otherwise decode ordinary actor data, where request-only `AckRequested`
   remains message metadata.

Dedicated ACK/NACK, Batch, and Stream oneofs never touch `data_frame()`.

### 10.2 Ordinary and batch delivery

Both build `TypedMessage` and `DeliveryOptions` through one internal helper so
sender, trace, message id, and flags cannot drift. Ordinary data makes one full
messaging call. Batch makes one full call per valid entry and aggregates
results. No fast path is used for ordinary or batch user messages.

### 10.3 RPC response

The router converts `RpcResponse` data to the existing `RpcResponseFrame` and
calls `RpcChannel::on_response()`. Spawn responses remain RPC-correlated
payloads; dead `ConnectionPool` spawn pre-parsing is not copied into the router.
Unknown correlation continues to follow `RpcChannel`'s existing behavior.

### 10.4 Backpressure and reliable control

Wire-specific parsing ends in the router. It passes typed decoded values to
Phase 3 messaging methods. `MessagingRuntime` and its coordinator no longer
receive `WireFrame` after Phase 4.

## 11. `StreamRuntime` Contract

Recommended private data model:

```cpp
enum class StreamSessionState : uint8_t {
    Opening,
    Active,
    Closing,
};

struct StreamKey {
    EndPoint peer;
    uint64_t stream_id{0};
    friend bool operator==(const StreamKey&, const StreamKey&) = default;
};

struct StreamSession {
    StreamSessionState state{StreamSessionState::Opening};
    ActorId sender_actor{};
    ActorId receiver_actor{};
    ActorId target_actor{};
    uint64_t generation{0};
};

enum class StreamDispatchCode : uint8_t {
    Handled,
    UnknownStream,
    DuplicateStream,
    CapacityExceeded,
    InvalidPeer,
    InvalidFrame,
    SpawnFailed,
    DeliveryRejected,
    MetadataDropped,
};

struct StreamDispatchResult {
    StreamDispatchCode code{StreamDispatchCode::Handled};
    uint32_t accepted_count{0};
    uint32_t rejected_count{0};
};

class StreamRuntime final {
  public:
    struct Config {
        uint32_t max_active_streams{4096};
        uint16_t max_tracestate_len{256};
    };

    result<StreamHandle> open(ActorId target, const StreamConfig& config);
    StreamDispatchResult on_open(const InboundFrameContext&,
                                 const net::StreamOpenFrame&) noexcept;
    StreamDispatchResult on_data(const InboundFrameContext&,
                                 const net::StreamDataFrame&) noexcept;
    StreamDispatchResult on_ack(const InboundFrameContext&,
                                const net::StreamAckFrame&) noexcept;
    StreamDispatchResult on_close(const InboundFrameContext&,
                                  const net::StreamCloseFrame&) noexcept;
    StreamDispatchResult on_error(const InboundFrameContext&,
                                  const net::StreamErrorFrame&) noexcept;
    StreamRuntimeSnapshot snapshot() const;
};
```

The concrete implementation uses one session map rather than separate sender
and receiver maps. A session can hold one or both local protocol actor ids.

### 11.1 Dependencies

- A narrow `StreamActorLifecyclePort` creates/stops sender and receiver actors
  through Phase 2's adoption path.
- `MessagingRuntime&` performs internal fast delivery with
  `FastDeliveryReason::StreamProtocol`.
- A fixed `StreamWirePort` is reserved for protocol output that must cross the
  network. It is bound to stable transitional network state and becomes a
  `NetworkRuntime` port in Phase 5.
- Metrics and endpoint configuration are fixed before start.

No port points to or captures `ActorSystem`/`Impl`.

The fixed control-path ports have explicit function/context shapes:

```cpp
struct StreamActorLifecyclePort {
    void* context{nullptr};
    result<ActorId> (*spawn_sender)(void*, ActorId target, uint64_t stream_id,
                                    const StreamConfig&,
                                    const TraceContext&) noexcept{nullptr};
    result<ActorId> (*spawn_receiver)(void*, ActorId target, uint64_t stream_id,
                                      const ActorAddress& sender,
                                      uint32_t initial_window_bytes,
                                      const TraceContext&) noexcept{nullptr};
    void (*stop)(void*, ActorId) noexcept{nullptr};
};

struct StreamWirePort {
    void* context{nullptr};
    bool (*send)(void*, const ActorAddress&,
                 const net::WireFrame&) noexcept{nullptr};
};
```

The actor port is required. The wire port may be unavailable while networking
is disabled; Phase 4 does not use that to claim remote `StreamHandle` support.

### 11.2 Registry lock rules

- Lock for capacity check, duplicate check, placeholder insertion, lookup,
  state transition, erase, and snapshot copy only.
- Copy `ActorId`/session metadata, then unlock before delivery.
- Reserve `Opening` before actor spawn.
- On spawn failure, erase the matching generation placeholder.
- On close/error, atomically mark/remove and copy both actor ids, then unlock
  and deliver terminal protocol messages.
- Never call user actor code directly.

### 11.3 Snapshot and operations safety

The snapshot is bounded by `max_active_streams` and contains registry-owned
facts only: peer, stream id, registry state, sender actor id, receiver actor id,
and target actor id. It does not read sender/receiver actor mutable window or
in-flight fields from an operations thread. Detailed actor state requires the
existing actor inspection/message mechanism in a later feature.

## 12. Public Facade and Compatibility

Preserved methods:

- `deliver_remote(const WireFrame&)`;
- `deliver_remote_batch(const WireFrame&)`;
- `register_stream_sender()`;
- `register_stream_receiver()`;
- `unregister_stream()`;
- `allocate_stream_id()`;
- `open_stream()`; and
- existing transport RPC/actor handler setters for direct transport users.

Facade behavior:

- `deliver_remote()` builds the best available compatibility context, invokes
  one router call, and preserves its `void` surface.
- `deliver_remote_batch()` invokes the same router; it is not a second batch
  implementation.
- stream registration/open methods forward to `StreamRuntime`.
- `open_stream()` converts the internal result to the existing optional.

An additive read-only stream registry snapshot may be exposed for CLI/admin.
`InboundFrameRouter` and `StreamRuntime` themselves remain private and are not
installed headers.

## 13. Concurrency and Threading Contract

- Connection framing and router invocation run on the existing event-loop
  callback thread.
- Different connection pools may invoke the sink concurrently if the network
  implementation evolves; router methods therefore contain no shared mutable
  state beyond an atomic accepting flag and thread-safe dependencies.
- Stream registry operations are mutex-protected as specified above.
- Actor delivery remains mailbox MPSC producer work; the scheduler is the sole
  actor mailbox consumer.
- The router never calls actor `receive()` or mutates actor state.
- Batch iteration is bounded and does not hold a stream/directory lock.
- Metrics/logging are bounded and do not block the event loop.
- Shutdown disables ingress before joining the event loop; no sink call may
  race router destruction.

## 14. Error and Failure Semantics

- Framing/decode errors do not enter downstream components.
- Unsupported/future payload oneofs are dropped with `UnsupportedPayload`,
  preserving the peer connection under current Phase 4 policy.
- Invalid flag combinations are rejected before any handler mutation.
- Ordinary delivery returns actor delivery status through the dispatch result;
  the public `void` adapter records it as existing policy permits.
- Batch processes all bounded entries and reports aggregate/partial outcome.
- Unknown stream frames never create registry state.
- Duplicate/capacity stream opens never replace an existing route.
- Failed stream actor spawn removes its opening reservation.
- A router disabled during drain returns `RuntimeStopping` and performs no
  downstream action.
- Phase 5 may map repeated typed failures to close/quarantine; Phase 4 does not
  introduce that transport policy.

## 15. Observability and Operations

Required preserved/connected signals:

- existing network frame received/decode-failed logs;
- existing batch frame/message metric event types;
- existing stream opened/closed/bytes/chunk/window metric event types where
  current protocol code supplies the value;
- delivery, reliable, backpressure, RPC, trace, and DLQ observations emitted by
  their owning components.

Recommended additive bounded codes:

- inbound dispatch result by `FrameDispatchCode`;
- decode failure by `FrameDecodeError`;
- unknown/duplicate/capacity stream frame counts;
- active/opening stream registry gauge; and
- batch accepted/rejected/invalid entry counts.

Structured logs may include peer, payload type, encoded size, reason code, and
stream id. They must not include actor message payload, stream chunk content,
trace state, or arbitrary remote error description. Remote descriptions are
bounded/sanitized before actor delivery and never used as metric labels.

CLI `stream/list` consumes the `StreamRuntime` snapshot. It must not lock the
registry while formatting and must not read mutable protocol actor fields.

## 16. Construction, Wiring, and Destruction

Construction dependencies first:

1. operations/telemetry and stable transitional network state;
2. `ActorRuntime`;
3. `MessagingRuntime`;
4. RPC channel and narrow stream actor/network ports;
5. `StreamRuntime`;
6. `InboundFrameRouter`;
7. install its fixed `InboundFrameSink` into transport before listen/run.

Shutdown/destruction:

1. mark readiness false and stop accepting new ingress;
2. disable router;
3. stop listening and stop/join the event loop/network thread;
4. clear/retire transport pools and callbacks;
5. destroy router;
6. close/snapshot/destroy stream runtime according to current shutdown policy;
7. destroy RPC, messaging, and actor dependencies only after their producers
   stop.

Member declaration order and the lifetime inventory must make reverse
destruction match this graph. A callback capturing a facade to “keep it simple”
is not permitted.

## 17. Source and Build Layout

Recommended public additive protocol types:

```text
include/hpactor/msg/frame.hpp
include/hpactor/net/frame_dispatch_result.hpp
include/hpactor/net/inbound_frame_sink.hpp
include/hpactor/actor/stream_snapshot.hpp
```

Private components:

```text
src/net/inbound_frame_router.hpp
src/net/inbound_frame_router.cpp
src/actor/stream_runtime.hpp
src/actor/stream_runtime.cpp
```

Existing implementation files modified in place:

```text
src/msg/frame.cpp
include/hpactor/net/connection_pool.hpp
src/net/connection_pool.cpp
include/hpactor/net/tcp_transport.hpp
src/net/tcp_transport.cpp
src/net/wireframe_connection.cpp
src/net/tls_connection.cpp
src/runtime/actor_system_impl.hpp
src/runtime/actor_system_impl.cpp
include/hpactor/actor/actor_system.hpp
src/actor/actor_system.cpp
src/cli/commands/stream_commands.cpp
```

Only lightweight result/sink/snapshot value types are public. Runtime component
headers remain private.

## 18. Incremental Migration Sequence

1. Inventory framing shapes, all payload/flag classifiers, handlers, stream
   map accesses, callback lifetimes, and current tests.
2. Add strict typed decode while preserving `WireFrame::decode()`.
3. Normalize plain/TLS callbacks to canonical encoded HPAC frames; add inbound
   size bounds and iterative resync.
4. Add fixed unified inbound sink to pools/transport with legacy fallback.
5. Implement router result types and classification against existing Phase 3
   messaging/RPC seams.
6. Correct reliable request/response flag ambiguity and support dedicated
   ACK/NACK oneofs.
7. Implement bounded ordered batch routing through full messaging delivery.
8. Implement the peer-qualified bounded `StreamRuntime` registry and snapshot.
9. Move stream handlers, correct protobuf/tag mapping, and add typed unknown/
   duplicate/capacity outcomes.
10. Route all stream oneofs through the router/runtime.
11. Replace facade frame/stream methods with compatibility forwards and remove
    stream state/private handlers.
12. Connect bounded observability/CLI snapshots and enforce architecture rules.
13. Verify normal, socket integration, stress, ASan, and TSAN behavior.

Each step is independently buildable. No commit may leave both old and new
production classifiers active for the same frame.

## 19. Verification Strategy

### 19.1 Unit tests

- every `FrameDecodeError`, exact-size success, empty/future envelope, and
  compatibility `decode()` behavior;
- size arithmetic and limit checks before allocation;
- every payload type and invalid flag combination;
- dedicated and legacy reliable mappings;
- ordinary request-only ACK metadata;
- batch metadata/order/aggregate outcomes and 1024-entry bound;
- peer-qualified stream key equality/hash and collision isolation;
- stream opening reservation, duplicate, capacity, spawn failure, and cleanup;
- protobuf-aware data/ACK/close/error TypeTag mapping;
- snapshot boundedness and no actor-state reads; and
- disabled router behavior.

### 19.2 Integration tests

- plain and TLS socket ingress deliver the same canonical frame;
- fragmented header/body, multiple frames in one read, invalid magic resync,
  oversize rejection, and decode failure reporting;
- RPC response reaches one `RpcChannel` handler only;
- ordinary reliable data with `AckRequested` reaches its actor;
- dual-bit legacy ACK, response-only NACK, and dedicated ACK/NACK update the
  correct Phase 3 tracker;
- batch entries reach the ordinary full-policy delivery path with partial
  result accounting;
- backpressure reaches the messaging coordinator;
- same stream id from two peers remains isolated;
- concurrent register/lookup/close/snapshot is race-free;
- stream close/error delivers internal protobuf tags once and erases once;
- facade `deliver_remote()` is a router forward; and
- shutdown joins callbacks before router destruction.

### 19.3 Architecture tests

- `InboundFrameRouter` is the only production valid-envelope classifier;
- `ConnectionPool` contains decode plus unified/legacy adapter selection, not
  payload business classification in unified mode;
- no production frame callback captures `ActorSystem`/`Impl`;
- `StreamRuntime` is the only production owner of stream map/counter state;
- no stream registry mutex crosses spawn/delivery/transport/log/metrics calls;
- no direct stream fast delivery occurs outside `StreamRuntime` and reviewed
  stream actor internals;
- `ActorSystem::deliver_remote()` and stream methods are forwards;
- no new `NetworkRuntime`, event-loop owner, generic handler registry, RTTI, or
  exceptions appear.

### 19.4 Sanitizers, stress, and compatibility

- ASan: malformed/truncated/oversize frames, opening rollback, terminal stream
  cleanup, disabled router, failed start, and destructor-only shutdown;
- TSAN: concurrent pools routing, stream open/data/close/snapshot, public
  registration versus network callbacks, and shutdown;
- stress: invalid-magic byte streams without recursion/stack growth, maximum
  batch, capacity-bound remote opens, repeated unknown stream frames;
- compatibility: old `decode()` behavior, legacy direct transport handlers,
  dual-bit ACK accepted by old classifier logic, current protobuf bytes and
  TypeTags unchanged.

## 20. Acceptance Criteria

Phase 4 is complete only when:

1. Plain/TLS frame handlers present one canonical encoded-frame shape and
   enforce the 16 MiB inbound bound before allocation.
2. Production ingress uses typed strict decode; malformed and unsupported are
   distinct and observable.
3. `InboundFrameRouter` solely classifies valid Data, Ack, Nack, Batch, and all
   Stream payload variants.
4. Ordinary, batch, reliable, backpressure, RPC, and stream paths reach exactly
   one owning subsystem.
5. Request-only `AckRequested` data is delivered; legacy/dedicated control is
   unambiguous and explicitly mapped.
6. Batch delivery preserves entry metadata/order, uses full messaging policy,
   is bounded, and reports partial outcomes.
7. `StreamRuntime` solely owns peer-qualified bounded stream session state,
   ids, handlers, and snapshots.
8. Stream registry locking follows the no-callback-under-lock contract and
   passes TSAN concurrency tests.
9. Data/ACK/close/error use correct protobuf serialization and internal tags;
   unknown/duplicate/capacity outcomes are typed and observable.
10. No stream maps/counters/frame handlers remain in `ActorSystem` or its
    generic shell.
11. `ActorSystem::deliver_remote()` and public stream methods are compatibility
    forwards only.
12. Callback stop/destruction ordering is tested; no facade capture remains.
13. CLI/admin sees bounded registry snapshots without shared actor-state reads.
14. No claim of complete remote `StreamHandle` behavior is made.
15. Focused/full tests and required ASan/TSAN scenarios pass.

## 21. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Framing normalization breaks a path that relied on stripped protobuf bytes | Define one canonical callback contract and test both plain/TLS socket paths |
| Typed decode duplicates connection framing | Connection finds exact frame boundary; decoder validates canonical bytes once; document both responsibilities |
| Router becomes another God Class | Stateless classification plus focused ordinary/batch helpers; policy remains in messaging/RPC/stream owners |
| Flag correction breaks rolling compatibility | Dual-bit ACK is accepted by old ACK-first code; retain legacy NACK and dedicated oneof support |
| Batch blocks event loop | Hard entry/frame bounds, no blocking work, per-entry bounded admission |
| Stream mutex serializes actor or transport work | Copy/transition under lock, invoke dependencies after unlock; architecture and TSAN tests |
| Peer-less facade tests cannot form a stream key | Derive compatibility context where possible; use local/unknown sentinel only for direct adapter tests |
| Opening reservation leaves orphan actor | Generation-checked rollback and narrow stop port |
| CLI reads mutable actor state | Registry snapshot only; detailed state via later actor inspection |
| Phase 4 expands into full streaming implementation | Explicit non-goal and acceptance criterion; fail unsupported remote open honestly |
| Router callback outlives dependencies | Disable ingress, join event loop, clear pools, then reverse-destroy router/dependencies |

## 22. Phase 5 Handoff

Phase 4 leaves a stable protocol boundary:

- connections produce typed decode events through `InboundFrameSink`;
- `InboundFrameRouter` returns fixed `FrameDispatchResult` values;
- `StreamRuntime` exposes typed inbound handlers and a fixed future wire-output
  port; and
- no protocol callback captures the facade.

Phase 5 `NetworkRuntime` takes ownership of transport, event loop/thread,
connection pools, discovery, timers, RPC/HTTP clients, remote spawn integration,
and sink installation. It maps repeated decode/dispatch results to peer policy
and guarantees start/stop idempotence.

Phase 5 must not move classification or stream registry state back into the
network owner. Phase 4 components remain concrete policy collaborators;
`NetworkRuntime` owns their I/O lifecycle, not their data-plane state.
