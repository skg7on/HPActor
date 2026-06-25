# MSG-008: Streaming Message Protocol — Design Spec

**Issue:** [#21](https://github.com/skg7on/HPActor/issues/21)  
**Date:** 2026-06-25  
**Status:** Approved  
**Subsystem:** Messaging  
**Priority:** P2  
**Release lane:** Maturity  
**Backlog refs:**
- `docs/architecture/production/architecture-requirement-backlog.md#L83` (MSG-008)
- `docs/architecture/production/feature-gap-refined-requirement-backlog.md#L461` (MSG-004 streaming half)

## 1. Motivation

Long-running actor data flows — telemetry streams, large result sets, progressive
computation — require lower per-chunk overhead than individual `send()` calls
and need receiver-driven flow control to prevent mailbox overflow. Today, every
chunk travels as a standalone `TypedMessage` through the full `DeliveryPipeline`
(pathological overhead for high-frequency flows) and there is no mechanism for
the receiver to signal "pause" or "resume" to the sender.

MSG-007 adds batch send/receive for one-shot bulk delivery. MSG-008 adds the
streaming half: a session-oriented protocol with credit-based flow control that
lets receivers throttle senders based on available mailbox capacity.

## 2. Scope

**In scope:**
- Stream session lifecycle: open → data → close/error
- Credit-based byte-window flow control (receiver-advertised window)
- `StreamHandle` API on `ActorContext` for user-facing stream operations
- Internal `StreamSenderActor` / `StreamReceiverActor` pair per stream (actor-backed, reusable scheduling/supervision/tracing)
- Wire protocol: 5 new frame types in `WireEnvelope` oneof
- Local fast path: direct mailbox enqueue between stream actors on same node
- Integration with mailbox backpressure (window shrinks/expands with mailbox pressure)
- Tracing: stream-level `TraceContext` propagated to all frames and delivered chunks
- Metrics: stream open/close counters, byte counters, window gauge
- CLI: `/stream list` and `/stream show <id>` commands

**Out of scope:**
- Stream retransmit (chunk-level ACK/NACK, selective retry) — follow-up MSG-008b
- Stream protocol negotiation / feature flags — assumes both peers speak stream frames
- Stream multiplexing over a single transport connection
- Durable stream state (persisted stream cursors for crash recovery)
- `DeliveryPipeline::try_deliver_stream_chunk()` — stream chunks bypass the pipeline (like MSG-007 batch)

## 3. Architecture

### 3.1 Overview

The streaming protocol adds three new concepts:

- **`StreamHandle`** — A move-only value type returned by `ActorContext::open_stream()`.
  The user's actor writes chunks, closes, or errors the stream through this handle.
  It is a thin facade — all protocol state lives in the internal stream actors.

- **`StreamSenderActor`** — An internal `EventBasedActor` spawned by the framework
  when a stream is opened. Owns the send buffer, credit window tracking, chunk
  sequencing, and idle timeout timer. Communicates with the remote
  `StreamReceiverActor` via the stream wire protocol.

- **`StreamReceiverActor`** — An internal `EventBasedActor` spawned by the framework
  on the receiving side. Owns the receive buffer, reassembly, window advertisement,
  and delivers completed chunks as regular `TypedMessage`s to the target actor's
  mailbox.

```
┌─────────────────────────┐                    ┌─────────────────────────┐
│ Sender Node             │                    │ Receiver Node           │
│                         │                    │                         │
│ User Actor              │                    │ User Actor              │
│   │                     │                    │   ▲                     │
│   │ open_stream()       │                    │   │ TypedMessage         │
│   ▼                     │                    │   │ (StreamChunkTag)     │
│ StreamHandle            │                    │   │                     │
│   │                     │                    │ StreamReceiverActor     │
│   │ write/close/error   │                    │   ▲                     │
│   ▼                     │                    │   │                     │
│ StreamSenderActor       │                    │   │ StreamDataFrame      │
│   │                     │                    │   │ StreamAckFrame       │
│   │ StreamDataFrame     │                    │   │ StreamCloseFrame     │
│   │ StreamOpenFrame     │    ────────────►   │   │                      │
│   │ StreamCloseFrame    │       wire         │   │                      │
│   │ StreamErrorFrame    │                    │   │                      │
│   │                     │                    │   │                      │
│   │ ◄── StreamAckFrame  │    ◄────────────   │   │                      │
│   │                     │                    │   │                      │
└─────────────────────────┘                    └─────────────────────────┘
```

**Local fast path:** When sender and receiver are on the same node, the frames
are enqueued directly to the `StreamReceiverActor`'s mailbox via
`try_push_batch()`, bypassing serialization and transport. Credit/ack frames
still flow through mailboxes (preserving backpressure) but skip the wire.

### 3.2 Component Responsibilities

| Component | Responsibility |
|-----------|---------------|
| `ActorContext` | `open_stream()` factory: validates target, spawns `StreamSenderActor`, returns `StreamHandle` |
| `StreamHandle` | User-facing API: `write()`, `close()`, `error()`. Forwards operations to `StreamSenderActor` via typed messages. Tracks `is_open()` state locally. |
| `StreamSenderActor` | Wire protocol send side: sequences chunks, tracks `bytes_in_flight`, honors `window_bytes`, manages send buffer with bounded capacity, runs idle timeout timer, spawns `StreamReceiverActor` on remote side via spawn request |
| `StreamReceiverActor` | Wire protocol receive side: reassembles chunks, advertises credit window (integrated with target mailbox pressure), delivers chunks as `TypedMessage`s to target actor, sends `StreamAckFrame` on chunk delivery and pressure change |
| `ActorSystem` | Route `StreamOpenFrame` to spawn `StreamReceiverActor` on target node; dispatch stream wire frames to correct stream actor |

## 4. Wire Protocol

### 4.1 Protobuf Changes (`protos/hpactor/frame.proto`)

```protobuf
// ── Stream protocol frames ────────────────────────────────────────────────

message StreamOpenFrame {
  uint64 stream_id = 1;
  hpactor.PbActorAddress sender = 2;
  hpactor.PbActorAddress receiver = 3;
  uint32 initial_window_bytes = 4;      // receiver → sender credit
  PbTraceContext trace_context = 5;     // optional stream-level trace
}

message StreamDataFrame {
  uint64 stream_id = 1;
  uint64 sequence = 2;                  // monotonic chunk sequence, starts at 1
  bytes payload = 3;                    // the chunk data
}

message StreamAckFrame {
  uint64 stream_id = 1;
  uint64 last_sequence = 2;             // cumulative ack: all chunks ≤ this received
  uint32 window_bytes = 3;              // updated credit window
}

message StreamCloseFrame {
  uint64 stream_id = 1;
  enum CloseReason {
    COMPLETE = 0;
    CANCELLED = 1;
    TIMEOUT = 2;
  }
  CloseReason reason = 2;
}

message StreamErrorFrame {
  uint64 stream_id = 1;
  uint32 error_code = 2;                // framework error code
  string description = 3;               // human-readable (max 256 chars)
}

// Extend WireEnvelope:
message WireEnvelope {
  oneof payload {
    ActorMsgFrame data_frame = 1;
    AckFrame ack_frame = 2;
    NackFrame nack_frame = 3;
    BatchMsgFrame batch_frame = 4;      // MSG-007
    StreamOpenFrame stream_open = 5;    // NEW
    StreamDataFrame stream_data = 6;    // NEW
    StreamAckFrame stream_ack = 7;      // NEW
    StreamCloseFrame stream_close = 8;  // NEW
    StreamErrorFrame stream_error = 9;  // NEW
  }
}
```

**Note on WireEnvelope migration:** The current `WireFrame` struct holds
`ActorMsgFrame` directly, not `WireEnvelope`. MSG-007 plans to migrate
`WireFrame` to use `WireEnvelope` as its internal representation. MSG-008
depends on this migration. If MSG-008 is implemented before MSG-007, the
`WireFrame` migration is done as part of MSG-008 instead.

### 4.2 Frame Flow

```
Sender                              Receiver
  │                                     │
  │──── StreamOpenFrame ──────────────► │  (stream_id=42, initial_window=64KB)
  │                                     │
  │◄─── StreamAckFrame ──────────────── │  (last_seq=0, window_bytes=64KB)
  │                                     │
  │──── StreamDataFrame(seq=1) ───────► │
  │──── StreamDataFrame(seq=2) ───────► │
  │──── StreamDataFrame(seq=3) ───────► │  (bytes_in_flight approaches window)
  │                                     │
  │◄─── StreamAckFrame ──────────────── │  (last_seq=3, window_bytes=32KB)
  │                                     │  (window shrunk — receiver pressure)
  │──── StreamDataFrame(seq=4) ───────► │
  │                                     │
  │◄─── StreamAckFrame ──────────────── │  (last_seq=4, window_bytes=0)
  │                                     │  (window=0 → sender MUST pause)
  │  [sender pauses, awaits credit]     │
  │                                     │
  │◄─── StreamAckFrame ──────────────── │  (last_seq=4, window_bytes=64KB)
  │                                     │  (receiver drained, more credit)
  │──── StreamDataFrame(seq=5) ───────► │
  │                                     │
  │──── StreamCloseFrame(COMPLETE) ───► │
  │                                     │
  │◄─── StreamAckFrame ──────────────── │  (final cumulative ack)
```

### 4.3 WireFrame Extension

New `PayloadType` discriminator values and factory methods:

```cpp
enum class PayloadType {
    Data, Ack, Nack, Batch,
    StreamOpen, StreamData, StreamAck, StreamClose, StreamError,
    Unknown
};

// New factories (alongside existing from_data/from_batch/from_ack/from_nack):
static WireFrame from_stream_open(StreamOpenFrame open);
static WireFrame from_stream_data(StreamDataFrame data);
static WireFrame from_stream_ack(StreamAckFrame ack);
static WireFrame from_stream_close(StreamCloseFrame close);
static WireFrame from_stream_error(StreamErrorFrame error);
```

### 4.4 Flow Control Contract

| Rule | Detail |
|------|--------|
| Sender must not exceed `window_bytes` in flight | `bytes_in_flight = sum(sent_chunk_sizes) - sum(acked_chunk_sizes)` |
| `window_bytes == 0` means STOP | Sender must pause until a non-zero window arrives |
| Window can shrink | Receiver may reduce window (backpressure). Sender must honor immediately |
| Cumulative ack | `last_sequence` acks all chunks ≤ that sequence. No selective ack |
| Receiver advertises window in every `StreamAckFrame` | Including initial response to `StreamOpenFrame` |
| Window integrates with mailbox pressure | Window shrinks when target mailbox exceeds high-watermark; expands when below low-watermark |
| Minimum window granularity | Window is in bytes; sender may send a single chunk even if its size exceeds remaining window (to avoid deadlock on large chunks). That chunk must be the last until credit is restored |

## 5. API Design

### 5.1 StreamHandle

```cpp
/// \brief Move-only handle for a streaming session.
///
/// Returned by \c ActorContext::open_stream(). The user's actor writes chunks,
/// closes the stream, or signals an error through this handle. All protocol
/// state (credit window, sequencing, send buffer) lives in the internal
/// \c StreamSenderActor.
///
/// \note \c write() is non-blocking — it enqueues into the sender actor's
///       bounded send buffer and returns immediately. If the send buffer is
///       full, \c write() returns \c false and the caller should apply
///       backpressure to its own producer.
class StreamHandle {
public:
    StreamHandle() = default;
    StreamHandle(ActorId sender_actor_id, uint64_t stream_id);
    ~StreamHandle();

    // Move-only
    StreamHandle(StreamHandle&& other) noexcept;
    StreamHandle& operator=(StreamHandle&& other) noexcept;
    StreamHandle(const StreamHandle&) = delete;
    StreamHandle& operator=(const StreamHandle&) = delete;

    /// Write a chunk to the stream.
    /// \return \c false if stream is closed, send buffer is full, or sender
    ///         actor is gone. \c true if the chunk was queued for send.
    bool write(TypedMessage chunk);

    /// Write raw bytes as a stream chunk.
    /// The chunk is delivered to the receiver with the original \p tag.
    bool write(TypeTag tag, StreamBuffer payload);

    /// Gracefully close the stream. Sends StreamCloseFrame(COMPLETE).
    /// \return \c false if already closed.
    bool close();

    /// Abort the stream with an error code.
    /// \return \c false if already closed.
    bool error(uint32_t code, std::string_view description = "");

    /// Number of bytes written but not yet acknowledged by the receiver.
    size_t bytes_in_flight() const;

    /// Current advertised receiver window in bytes.
    size_t window_bytes() const;

    /// True if the stream is open (not yet closed or errored).
    bool is_open() const;

    /// Unique stream identifier.
    uint64_t stream_id() const { return stream_id_; }

private:
    ActorId sender_actor_id_;
    uint64_t stream_id_ = 0;
    bool closed_ = false;
};
```

### 5.2 StreamConfig

```cpp
/// \brief Configuration for a streaming session.
struct StreamConfig {
    /// Initial credit window in bytes advertised by the receiver.
    uint32_t initial_window_bytes = 64 * 1024;  // 64 KiB

    /// Maximum size of a single stream chunk payload (excluding framing).
    uint32_t max_chunk_bytes = 64 * 1024;       // 64 KiB

    /// Maximum bytes the sender will buffer before write() returns false.
    uint32_t send_buffer_bytes = 256 * 1024;    // 256 KiB

    /// Idle timeout — if no data or ack frame is received within this
    /// duration, the stream is errored with TIMEOUT.
    Duration idle_timeout = Duration::from_seconds(30);

    /// Maximum number of stream data frames that may be in flight before
    /// the sender pauses (additional cap beyond byte-window).
    uint32_t max_in_flight_frames = 256;
};
```

### 5.3 ActorContext Extension

```cpp
/// \brief Open a streaming session to a target actor.
///
/// Spawns an internal \c StreamSenderActor that manages the stream protocol
/// (credit window, sequencing, idle timeout). On the receiver side, a
/// \c StreamReceiverActor is spawned (on the target node, or locally if same
/// node) to handle reassembly and chunk delivery.
///
/// \param[in] target Destination actor ID.
/// \param[in] config Stream configuration.
/// \return \c StreamHandle for writing chunks, or \c std::nullopt if the
///         stream could not be opened (target unreachable, config invalid,
///         receiver actor not found).
std::optional<StreamHandle> open_stream(ActorId target,
                                        StreamConfig config = {});
```

### 5.4 Receiver-Side TypeTags

Stream lifecycle events are delivered to the receiving actor as `TypedMessage`s
with subsystem-scoped type tags. These are declared as `constexpr` variables in
the stream subsystem header, using the `make_subsystem_tag()` mechanism
(extension range `0x80–0xFF`):

```cpp
namespace hpactor::stream {

/// \brief Stream data chunk delivered to the receiver actor.
///
/// The payload carries the original chunk data. The message's \c sender_address
/// identifies the stream sender. The stream's trace context is attached.
inline constexpr TypeTag StreamChunkTag = make_subsystem_tag(0x80);

/// \brief Stream session has been opened.
///
/// Delivered before the first chunk. Payload: \c StreamOpenedPayload
/// (stream_id, sender address, config summary).
inline constexpr TypeTag StreamOpenedTag = make_subsystem_tag(0x81);

/// \brief Stream has been gracefully closed by the sender.
///
/// Payload: \c StreamClosedPayload (stream_id, close reason, total bytes).
inline constexpr TypeTag StreamClosedTag = make_subsystem_tag(0x82);

/// \brief Stream has errored.
///
/// Payload: \c StreamErrorPayload (stream_id, error code, description).
inline constexpr TypeTag StreamErrorTag = make_subsystem_tag(0x83);

} // namespace hpactor::stream
```

The receiving actor registers handlers for these tags like any other message:

```cpp
class MyActor : public EventBasedActor {
    Behavior make_behavior() override {
        return Behavior{
            // Stream lifecycle events
            on_stream_opened([](const StreamOpenedPayload& p) { /* ... */ }),
            on_stream_closed([](const StreamClosedPayload& p) { /* ... */ }),
            on_stream_error([](const StreamErrorPayload& p) { /* ... */ }),
        };
    }
    // Chunks arrive as regular TypedMessage with the original TypeTag
};
```

### 5.5 Local Fast Path

When `open_stream()` detects the target is on the same node:

```
context()->open_stream(local_id, config)
    → spawn StreamSenderActor (local)
    → spawn StreamReceiverActor (local)
    → StreamSenderActor enqueues StreamDataFrames directly to
      StreamReceiverActor's mailbox via try_push_batch()
    → No serialization, no transport
    → StreamAckFrames flow back through StreamSenderActor's mailbox
```

Same pattern as `try_deliver_local_fast()` for single messages and the local
fast path in MSG-007 for batch delivery.

## 6. Error Handling & Lifecycle

### 6.1 Stream States

```
          open_stream()
               │
               ▼
          ┌────────┐
          │ OPENING │── StreamOpenFrame sent, awaiting first ack
          └────┬───┘
               │ first StreamAckFrame received
               ▼
          ┌──────────┐
          │ STREAMING │◄── data/ack frames flowing
          └────┬─────┘
               │
          ┌────┴────────────┐
          ▼                 ▼
     ┌─────────┐      ┌─────────┐
     │ CLOSING │      │  ERROR  │
     └────┬────┘      └────┬────┘
          │                 │
          ▼                 ▼
     ┌─────────┐      ┌─────────┐
     │ CLOSED  │      │(terminal)│
     └─────────┘      └─────────┘
```

### 6.2 Error Scenarios

| Scenario | Detected by | Action |
|----------|-------------|--------|
| **Receiver unreachable** | StreamSenderActor on open | `open_stream()` returns `std::nullopt`; no actors spawned |
| **Receiver actor gone mid-stream** | StreamSenderActor on send failure | Sends `StreamErrorFrame`, `is_open()` becomes false, error delivered to receiver if reachable |
| **Sender actor terminates** | ActorSystem on actor exit | All open streams owned by exiting actor are closed with `CANCELLED` reason; stream actors self-terminate |
| **Idle timeout** | StreamSenderActor timer | After `idle_timeout` with no data/ack, sends `StreamErrorFrame(TIMEOUT)`; stream terminated |
| **Window starvation** | StreamSenderActor | If `window_bytes == 0` for longer than `idle_timeout`, treated as timeout |
| **Receiver mailbox full** | StreamReceiverActor | Window shrinks to 0 in `StreamAckFrame`; sender pauses; chunks already sent may be dropped → DLQ |
| **Out-of-order sequence** | StreamReceiverActor | Sends `StreamErrorFrame`, drops stream, delivers error to receiver actor |
| **Corrupt frame** | `WireFrame::decode()` | Frame dropped; no ack sent; sender will retransmit on timeout (retransmit deferred to MSG-008b) |

### 6.3 Cleanup Contract

- When a stream closes or errors, both `StreamSenderActor` and `StreamReceiverActor`
  self-terminate after notifying their respective user actors
- `StreamHandle::is_open()` returns `false` after close/error
- `write()` on a closed handle returns `false` (no-op, no crash)
- If the user actor holding a `StreamHandle` terminates without calling `close()`,
  `StreamHandle`'s destructor sends a close signal to the `StreamSenderActor`,
  which sends `StreamCloseFrame(CANCELLED)` and self-terminates

## 7. Integration with Existing Infrastructure

### 7.1 Supervision

Stream actors are spawned as children of the user's actor that called
`open_stream()`. If a stream actor fails, the supervisor strategy applies
(OneForOne, AllForOne, etc.). Stream state is not preserved across restarts —
the stream is errored and must be re-established by the application.

### 7.2 Tracing

- `StreamOpenFrame` carries a `TraceContext` (from the sender's current trace scope).
- All subsequent frames for that stream inherit the same trace context.
- Each chunk delivered to the receiver actor carries the stream's trace context.
- `StreamReceiverActor` creates a `Span` (consumer kind) for the stream session.

### 7.3 Dead Letter Queue

| Condition | DLQ Action |
|-----------|------------|
| Chunk arrives after stream closed/errored | Dead-lettered with `DeadLetterReason::StreamClosed` |
| Chunk dropped due to receiver mailbox overflow | Dead-lettered with existing `DeadLetterReason::MailboxOverflow` |
| Stream open request for unknown target | Dead-lettered with `DeadLetterReason::ActorNotFound` |

New `DeadLetterReason` value: `StreamClosed = 39`.

### 7.4 Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `hpactor_streams_opened_total` | Counter | Stream sessions opened (sender side) |
| `hpactor_streams_accepted_total` | Counter | Stream sessions accepted (receiver side) |
| `hpactor_streams_closed_total` | Counter | Streams closed, labeled by reason (complete/cancelled/timeout/error) |
| `hpactor_stream_bytes_sent_total` | Counter | Total bytes sent across all streams |
| `hpactor_stream_bytes_received_total` | Counter | Total bytes received across all streams |
| `hpactor_stream_window_bytes` | Gauge | Current advertised receiver window (per-stream or aggregate) |
| `hpactor_stream_bytes_in_flight` | Gauge | Current bytes in flight (sender side) |
| `hpactor_stream_chunks_sent_total` | Counter | Total stream data frames sent |
| `hpactor_stream_chunks_received_total` | Counter | Total stream data frames received |

New metric event types: `kStreamOpened`, `kStreamClosed`, `kStreamBytesSent`,
`kStreamBytesReceived`.

### 7.5 CLI

```cpp
// Commands registered in src/cli/commands/stream_commands.cpp:

/// /stream list — enumerate active streams
/// Output: stream_id, sender actor, receiver actor, state, bytes_in_flight, window_bytes

/// /stream show <stream_id> — detailed stream state
/// Output: full stream metadata, frame counters, window history, timing
```

Stream actors expose state via `InspectStateRequest`/`InspectStateReply` — the
existing `serialize_state()` / `mailbox_snapshot()` virtual interface on
`AbstractActor`.

### 7.6 Fault Injection

New fault domain `FaultDomain::Stream` with fault points:

| Fault Point | Action | Description |
|-------------|--------|-------------|
| `hpactor.stream.open` | Fail, Drop, Delay | StreamOpenFrame send failure or drop |
| `hpactor.stream.data` | Fail, Drop, Delay | StreamDataFrame send failure or drop |
| `hpactor.stream.ack` | Fail, Drop, Delay | StreamAckFrame failure or drop |
| `hpactor.stream.close` | Fail, Drop | StreamCloseFrame failure or drop |
| `hpactor.stream.error` | Fail, Drop | StreamErrorFrame failure or drop |

## 8. Stream ID Allocation

Stream IDs are allocated by the sender side using a monotonic counter per
`ActorSystem` instance, combined with the sender's `ActorId` to ensure global
uniqueness:

```
stream_id = (sender_actor_id << 32) | (monotonic_counter++)
```

This avoids coordination between sender and receiver for ID allocation while
guaranteeing uniqueness across all active streams in the system.

## 9. Wire Frame Routing

### 9.1 Frame Dispatch in ActorSystem

```
deliver_remote(frame)
    → switch (frame.payload_type())
        case StreamOpen:  → deliver_remote_stream_open(frame)
        case StreamData:  → deliver_remote_stream_data(frame)
        case StreamAck:   → deliver_remote_stream_ack(frame)
        case StreamClose: → deliver_remote_stream_close(frame)
        case StreamError: → deliver_remote_stream_error(frame)
        // Existing cases for Data, Ack, Nack, Batch unchanged
```

### 9.2 Stream Actor Lookup

The `ActorSystem` maintains an internal map of `stream_id → StreamSenderActor`
(for routing inbound `StreamAckFrame`s back to the sender) and
`stream_id → StreamReceiverActor` (for routing inbound `StreamDataFrame`s to the
receiver). This is a lightweight `std::unordered_map<uint64_t, ActorId>` with
O(1) lookup.

## 10. Testing Strategy

### 10.1 Unit Tests

| Test | What it validates |
|------|-------------------|
| `StreamOpenFrame roundtrip` | Encode → decode preserves all fields |
| `StreamDataFrame roundtrip` | Encode → decode preserves sequence + payload |
| `StreamAckFrame roundtrip` | Encode → decode preserves last_seq + window |
| `StreamCloseFrame roundtrip` | Encode → decode preserves reason |
| `StreamErrorFrame roundtrip` | Encode → decode preserves code + description |
| `WireEnvelope stream discrimination` | Envelope with stream oneof fields decodes correctly |
| `Stream ID uniqueness` | Consecutive allocations produce unique IDs |
| `StreamConfig defaults` | Default config values are sensible |
| `StreamHandle move semantics` | Move-construct and move-assign work correctly |
| `StreamHandle closed write` | write() on closed handle returns false |
| `StreamHandle double close` | close() on already-closed handle returns false |
| `Credit window arithmetic` | bytes_in_flight calculation with various send/ack patterns |
| `Window zero pause` | Sender state machine enters pause when window hits 0 |

### 10.2 Integration Tests

| Test | What it validates |
|------|-------------------|
| `stream open local` | open_stream() spawns sender + receiver actors on same node |
| `stream open remote` | open_stream() spawns receiver on remote node via SpawnReceiver |
| `stream data flow local` | Chunks sent via StreamHandle arrive at receiver actor |
| `stream data flow remote` | Chunks traverse wire → arrive at remote receiver actor |
| `stream close graceful` | StreamCloseFrame(COMPLETE) → receiver gets StreamClosedTag |
| `stream error abort` | StreamErrorFrame → receiver gets StreamErrorTag |
| `stream backpressure` | Receiver mailbox fills → window shrinks → sender pauses |
| `stream window zero` | Receiver advertises window=0 → sender stops sending |
| `stream window resume` | Window reopens → sender resumes sending |
| `stream idle timeout` | No ack within idle_timeout → stream errored |
| `stream sender actor exit` | User actor terminates → streams closed with CANCELLED |
| `stream receiver actor gone` | Receiver actor exits → sender detects, stream errored |
| `stream trace propagation` | TraceContext flows from open → data frames → receiver chunks |
| `stream metrics` | Counters/gauges increment correctly during lifecycle |
| `stream cli list` | `/stream list` shows active streams |
| `stream cli show` | `/stream show <id>` shows detailed state |
| `stream local fast path` | Same-node streams bypass serialization |
| `stream fault injection` | FAULT_INJECT at stream.data drop → sender pauses, stream survives |

### 10.3 System Tests

| Test | What it validates |
|------|-------------------|
| `stream multi_chunk_large_transfer` | 1000 chunks of 4KB over remote stream → all arrive in order |
| `stream concurrent_streams` | 10 concurrent streams between same actor pair → no interference |
| `stream shutdown_during_stream` | ActorSystem shutdown with active streams → clean termination |

## 11. Files Changed

| File | Change |
|------|--------|
| `protos/hpactor/frame.proto` | Add `StreamOpenFrame`, `StreamDataFrame`, `StreamAckFrame`, `StreamCloseFrame`, `StreamErrorFrame`; extend `WireEnvelope` oneof |
| `include/hpactor/msg/frame.hpp` | Add `PayloadType` enum values; add stream factory methods; migrate to `WireEnvelope` (or depend on MSG-007 migration) |
| `include/hpactor/msg/stream_handle.hpp` | **New:** `StreamHandle` class definition |
| `include/hpactor/actor/stream_sender_actor.hpp` | **New:** `StreamSenderActor` class |
| `include/hpactor/actor/stream_receiver_actor.hpp` | **New:** `StreamReceiverActor` class |
| `include/hpactor/actor/stream_config.hpp` | **New:** `StreamConfig` struct |
| `include/hpactor/actor/stream_types.hpp` | **New:** `StreamOpenedPayload`, `StreamClosedPayload`, `StreamErrorPayload`, stream TypeTag declarations |
| `include/hpactor/actor/actor_context.hpp` | Add `open_stream()` declaration |
| `include/hpactor/core/actor_system.hpp` | Add stream actor registry, stream frame dispatch methods |
| `src/actor/stream_sender_actor.cpp` | **New:** `StreamSenderActor` implementation |
| `src/actor/stream_receiver_actor.cpp` | **New:** `StreamReceiverActor` implementation |
| `src/actor/actor_context.cpp` | Add `open_stream()` implementation |
| `src/actor/actor_system.cpp` | Add stream frame dispatch, stream actor registry |
| `src/net/frame.cpp` | Add stream frame encode/decode, `PayloadType` dispatch |
| `src/metrics/metrics_event.hpp` | Add stream metric event types |
| `src/metrics/metrics_aggregator.cpp` | Wire stream metric aggregation |
| `src/cli/commands/stream_commands.cpp` | **New:** `/stream list`, `/stream show` CLI commands |
| `src/fault/fault_points.cpp` | Add `FaultDomain::Stream` fault points |
| `tests/unit/msg/test_stream_frames.cpp` | **New:** Stream frame roundtrip unit tests |
| `tests/unit/actor/test_stream_handle.cpp` | **New:** StreamHandle unit tests |
| `tests/integration/actor/test_stream_messaging.cpp` | **New:** Stream integration tests |
| `tests/system/test_stream_system.cpp` | **New:** Stream system tests |

## 12. Dependencies

- **MSG-007 WireEnvelope migration:** MSG-008 relies on `WireFrame` holding
  `WireEnvelope` (with oneof dispatch) rather than raw `ActorMsgFrame`. This
  migration is planned in MSG-007. If MSG-008 is implemented first, the
  migration is done here.
- **SpawnReceiver:** Remote stream setup uses the existing `SpawnReceiver`
  infrastructure to spawn `StreamReceiverActor` on the target node.
- **MultiLaneQueue:** Stream control frames (open, ack, close, error) use the
  system lane; stream data frames use the user lane with priority routing.
- **Bounded mailbox admission:** `StreamReceiverActor` monitors target mailbox
  pressure to compute advertised window size.
