# Streaming Message Protocol — Core Concept and Architecture Design

**Runtime ownership:** Stream session state, id allocation, and protocol
handlers are owned by `StreamRuntime`. Inbound stream frames are classified by
`InboundFrameRouter` on the network-loop thread and dispatched to
`StreamRuntime`. See
[actor-system-runtime-architecture.md](actor-system-runtime-architecture.md) for
the full component graph and ownership contracts.

## 1. Executive Summary

The streaming message protocol gives HPActor a session-oriented, flow-controlled
channel for long-running actor data flows — telemetry streams, bulk data transfers,
progressive computation results, and continuous event feeds. Unlike individual
`send()` calls where every message travels through the full `DeliveryPipeline`
independently, a stream establishes a lightweight session between sender and
receiver with credit-based byte-window flow control. The receiver advertises how
many bytes it can accept; the sender must not exceed that window. This prevents
the receiver's mailbox from overflowing and gives the receiver direct control
over ingestion rate.

**Key Design Decisions:**

- **Actor-backed sessions**: Every stream is backed by an internal
  `StreamSenderActor` / `StreamReceiverActor` pair spawned by the framework.
  Protocol state, credit tracking, sequencing, and timers live in actors —
  reusing the existing mailbox, scheduling, supervision, tracing, and CLI
  infrastructure without adding new concurrency primitives.

- **`StreamRuntime` ownership**: The stream registry is a peer-qualified bounded
  session map (`StreamKey{EndPoint, uint64_t}`, max 4096 active streams)
  protected by a dedicated mutex. Two-phase open (reserve Opening → spawn actors
  → commit Active) with rollback. Network callbacks look up/remove entries under
  the mutex; all callers release the lock before actor spawning, delivery,
  transport calls, or callbacks.

- **`InboundFrameRouter` classification**: Stream frames are routed by oneof
  type (StreamOpen/Data/Ack/Close/Error) from the single
  `InboundFrameRouter::dispatch()` entry point. No stream protocol demultiplexing
  remains in `ActorSystem`.

- **Credit-based byte-window flow control**: The receiver advertises
  `window_bytes` in every `StreamAckFrame`. The sender tracks `bytes_in_flight`
  and pauses when the window is exhausted. The window integrates with mailbox
  pressure: it shrinks when the target mailbox exceeds its high-watermark and
  expands when it drains below the low-watermark.

- **Thin user-facing API**: `ActorContext::open_stream()` returns a move-only
  `StreamHandle` with `write()`, `close()`, and `error()` methods. The handle is
  a facade — all protocol mechanics (credit, sequencing, retransmit, timeout)
  are owned by the internal stream actors.

- **Remote-first, local fast-path**: The wire protocol defines five frame types
  (`StreamOpenFrame`, `StreamDataFrame`, `StreamAckFrame`, `StreamCloseFrame`,
  `StreamErrorFrame`) in the `WireEnvelope` oneof. On the same node, frames
  bypass serialization and transport, enqueuing directly to the receiver actor's
  mailbox via `try_push_batch()`.

- **Envelope-level trace propagation**: The stream's `TraceContext` is set at
  open time and carried on every frame. Chunks delivered to the receiver actor
  inherit the stream's trace, so the entire data flow is one traced operation.

The streaming protocol is a P2 (Maturity) feature. It depends on the
`WireEnvelope` oneof dispatch migration (planned in MSG-007) and integrates
with the existing `SpawnReceiver` for remote stream setup.

---

## 2. Problem Statement

HPActor actors communicate via `context()->send(target, msg)` — one message, one
delivery. This works well for request-response and event-driven workloads, but
breaks down for long-running data flows:

1. **Per-message overhead**: Each chunk travels through the full
   `DeliveryPipeline` (dedup, circuit breaker, TTL, backpressure, admission).
   For a stream of 10,000 small chunks, this is 10,000 pipeline traversals
   when one session establishment is sufficient.

2. **No receiver-driven flow control**: The sender has no way to know whether
   the receiver can keep up. A fast producer can overflow a slow consumer's
   mailbox, causing dropped messages, DLQ churn, or OOM risk — even with
   bounded mailboxes.

3. **No session semantics**: There is no concept of "open a channel, send data,
   close the channel." The receiver cannot distinguish "the sender is done"
   from "there are more messages coming later." This makes it hard to build
   scatter-gather, progressive result, or ordered bulk transfer patterns.

4. **No backpressure propagation**: Bounded mailboxes provide local admission
   control, but backpressure signals (`StreamPause` / `StreamResume`) don't
   exist. A sender learns about receiver pressure only indirectly through
   delivery failures or DLQ entries.

The batch protocol (MSG-007) solves the overhead problem for one-shot bulk
sends. The streaming protocol solves the session, flow-control, and
backpressure problems for continuous flows.

---

## 3. Goals and Non-Goals

### Goals

- Provide a session-oriented stream abstraction: open → data → close/error.
- Credit-based flow control where the receiver controls the sender's rate.
- Window advertisement integrated with mailbox pressure (high/low watermarks).
- Local fast path that bypasses serialization and transport on same-node streams.
- Trace propagation across the entire stream lifecycle.
- Observable via metrics, CLI, and dead-letter queue integration.
- Deterministic fault injection points for chaos testing.

### Non-Goals

- Chunk-level retransmit (ACK/NACK per chunk, selective retry) — deferred to MSG-008b.
- Protocol negotiation / feature flags — initial implementation assumes both
  peers speak stream frames.
- Stream multiplexing over a single transport connection.
- Durable stream cursors for crash recovery.
- Auto-batching of individual `send()` calls into streams.

---

## 4. Design Philosophy

### Streams Are Actors

Every stream session is a pair of framework-managed actors. The user never
spawns or manages `StreamSenderActor` or `StreamReceiverActor` directly — the
framework does it through `ActorContext::open_stream()`. This has several
advantages:

- **Mailbox integration**: Stream frames flow through the same `MultiLaneQueue`
  as all other messages. Control frames (open, ack, close, error) use the
  system lane; data frames use the user lane. Flow control is a natural
  consequence of mailbox admission — when the receiver's mailbox is full, the
  window shrinks to zero.

- **Supervision for free**: Stream actors are children of the user's actor.
  If a stream actor fails, the supervisor strategy applies. A failing stream
  doesn't crash the user's actor.

- **CLI visibility**: `/stream list` and `/stream show <id>` work through the
  existing `InspectState` interface. No new introspection mechanism needed.

- **Tracing for free**: Stream actors create spans through the existing trace
  infrastructure. The stream lifecycle (open, each chunk, close) is one traced
  operation.

### Credit Window, Not Rate Limiting

The receiver doesn't tell the sender "send at most N chunks per second."
It tells the sender "you may have up to N bytes in flight at any moment."
This is a window, not a rate:

- When the receiver is fast, the window stays wide open and the sender streams
  at line rate.
- When the receiver slows down (mailbox filling), the window shrinks
  proportionally. The sender backs off automatically.
- When the receiver catches up (mailbox draining), the window reopens.
- A window of zero means "stop entirely" — not "slow down."

This maps naturally to the existing bounded mailbox watermarks: above the
high-watermark, the advertised window shrinks toward zero. Below the
low-watermark, it expands back toward the configured maximum.

### Bounded Everything

Every buffer in the stream protocol has explicit bounds:

| Buffer | Bound | Behavior when full |
|--------|-------|--------------------|
| Sender send buffer | `send_buffer_bytes` (default 256 KiB) | `write()` returns `false` |
| Receiver advertised window | `initial_window_bytes` (default 64 KiB) | Sender pauses |
| In-flight frames | `max_in_flight_frames` (default 256) | Sender pauses |
| Chunk payload | `max_chunk_bytes` (default 64 KiB) | `write()` rejects oversize chunk |

There are no unbounded queues. Every backpressure path is explicit and
observable.

### Cumulative Ack, Not Selective

The receiver acknowledges all chunks up to a sequence number: "I have
everything ≤ seq N." This is simpler than selective ack (bitmap of received
chunks) and sufficient for reliable ordered delivery over TCP transports.
Selective ack can be added later if UDP transport or high-loss environments
require it (MSG-008b).

---

## 5. Architecture Overview

```text
┌─────────────────────────┐                    ┌─────────────────────────┐
│ Sender Node             │                    │ Receiver Node           │
│                         │                    │                         │
│ User Actor              │                    │ User Actor              │
│   │                     │                    │   ▲                     │
│   │ open_stream()       │                    │   │ TypedMessage         │
│   ▼                     │                    │   │ (StreamChunkTag,     │
│ StreamHandle            │                    │   │  StreamClosedTag,    │
│   │                     │                    │   │  StreamErrorTag)     │
│   │ write/close/error   │                    │   │                     │
│   ▼                     │                    │ StreamReceiverActor     │
│ StreamSenderActor       │                    │   ▲                     │
│   │                     │                    │   │ frames from          │
│   │ StreamDataFrame     │                    │   │ ActorSystem dispatch │
│   │ StreamOpenFrame     │    ────────────►   │   │                     │
│   │ StreamCloseFrame    │       wire         │   │                     │
│   │ StreamErrorFrame    │                    │   │                     │
│   │                     │                    │   │                     │
│   │ ◄── StreamAckFrame  │    ◄────────────   │   │                     │
│   │                     │                    │   │                     │
└─────────────────────────┘                    └─────────────────────────┘
```

### Lifecycle

```text
 open_stream()
      │
      ▼
 ┌────────┐
 │ OPENING │── StreamOpenFrame sent, awaiting first ack
 └────┬───┘
      │ first StreamAckFrame received
      ▼
 ┌──────────┐
 │ STREAMING │◄── data/ack frames flowing, window tracking active
 └────┬─────┘
      │
 ┌────┴────────────┐
 ▼                 ▼
┌─────────┐   ┌─────────┐
│ CLOSING │   │  ERROR  │
└────┬────┘   └─────────┘
     │              │
     ▼              ▼
┌─────────┐   (terminal)
│ CLOSED  │
└─────────┘
```

### Ownership Model

```text
ActorSystem
  │
  ├── stream_id → StreamSenderActor map (for routing inbound StreamAckFrames)
  ├── stream_id → StreamReceiverActor map (for routing inbound StreamDataFrames)
  │
  └── ActorSystem::deliver_remote()
        └── switch (frame.payload_type())
              ├── StreamOpen  → spawn StreamReceiverActor, deliver StreamOpenedTag
              ├── StreamData  → route to StreamReceiverActor → deliver chunk
              ├── StreamAck   → route to StreamSenderActor → update window
              ├── StreamClose → route to StreamReceiverActor → deliver StreamClosedTag
              └── StreamError → route to both → deliver StreamErrorTag, terminate

User Actor
  │
  ├── owns StreamHandle (move-only value, returned by open_stream())
  │     └── write(chunk) → sends message to StreamSenderActor
  │     └── close()      → sends close signal to StreamSenderActor
  │     └── error(code)  → sends error signal to StreamSenderActor
  │
  └── supervises StreamSenderActor (child actor)
        └── if stream actor fails, supervisor strategy applies
```

### Data Flow (Send Side)

```text
User code: handle->write(chunk)
  │
  │ message to StreamSenderActor's mailbox
  ▼
StreamSenderActor::receive()
  │
  ├── Check: is stream open? bytes_in_flight < window_bytes?
  │     └── No → buffer or reject (write() returns false if buffer full)
  │
  ├── Assign sequence number, compute bytes_in_flight
  │
  ├── Local target?
  │     └── Yes → enqueue StreamDataFrame directly to StreamReceiverActor mailbox
  │
  └── Remote target?
        └── encode StreamDataFrame → WireEnvelope → transport->try_send()
```

### Data Flow (Receive Side)

```text
StreamReceiverActor::receive()  ← StreamDataFrame from ActorSystem dispatch
  │
  ├── Validate sequence (must be last_delivered + 1)
  │     └── Gap/duplicate → send StreamErrorFrame, terminate
  │
  ├── Deliver chunk as TypedMessage to target actor's mailbox
  │     └── attach stream's TraceContext to the message
  │
  ├── Update mailbox pressure gauge
  │     └── above high-watermark → shrink advertised window
  │     └── below low-watermark  → expand advertised window
  │
  └── Send StreamAckFrame(last_seq, window_bytes) to StreamSenderActor
```

---

## 6. Wire Protocol

Five frame types extend the `WireEnvelope` oneof (alongside existing
`data_frame`, `ack_frame`, `nack_frame`, and MSG-007's `batch_frame`):

| Frame | Direction | Purpose | Key Fields |
|-------|-----------|---------|------------|
| `StreamOpenFrame` | Sender → Receiver | Establish session | `stream_id`, `sender`, `receiver`, `initial_window_bytes`, `trace_context` |
| `StreamDataFrame` | Sender → Receiver | Deliver a chunk | `stream_id`, `sequence` (monotonic), `payload` |
| `StreamAckFrame` | Receiver → Sender | Acknowledge + advertise window | `stream_id`, `last_sequence` (cumulative), `window_bytes` |
| `StreamCloseFrame` | Either → Either | Graceful close | `stream_id`, `reason` (COMPLETE/CANCELLED/TIMEOUT) |
| `StreamErrorFrame` | Either → Either | Abort on error | `stream_id`, `error_code`, `description` |

The `stream_id` is globally unique per session, allocated by the sender as
`(sender_actor_id << 32) | monotonic_counter++`.

### Frame Flow Example

```text
Sender                              Receiver
  │                                     │
  │──── StreamOpenFrame ──────────────► │  stream_id=42, window=64KB
  │◄─── StreamAckFrame ──────────────── │  last_seq=0, window=64KB
  │                                     │
  │──── StreamDataFrame(seq=1) ───────► │
  │──── StreamDataFrame(seq=2) ───────► │
  │──── StreamDataFrame(seq=3) ───────► │
  │◄─── StreamAckFrame ──────────────── │  last_seq=3, window=32KB (pressure)
  │                                     │
  │──── StreamDataFrame(seq=4) ───────► │
  │◄─── StreamAckFrame ──────────────── │  last_seq=4, window=0 (STOP)
  │  [sender pauses]                    │
  │◄─── StreamAckFrame ──────────────── │  last_seq=4, window=64KB (resume)
  │──── StreamDataFrame(seq=5) ───────► │
  │                                     │
  │──── StreamCloseFrame(COMPLETE) ───► │
  │◄─── StreamAckFrame ──────────────── │  final cumulative ack
```

---

## 7. Flow Control Contract

The receiver controls the sender's rate through the advertised `window_bytes`:

| Rule | Detail |
|------|--------|
| **Window is a byte cap** | `bytes_in_flight = sum(sent) - sum(acked)`. Must not exceed `window_bytes`. |
| **Zero means stop** | `window_bytes == 0` → sender must pause all data frames. |
| **Window can shrink** | The receiver may reduce the window at any time. Sender must honor immediately. |
| **Cumulative ack** | `last_sequence` acknowledges all chunks ≤ that number. |
| **Window driven by mailbox pressure** | Receiver monitors target mailbox depth. Window shrinks above high-watermark; expands below low-watermark. |
| **Anti-deadlock** | A single chunk may be sent even if it exceeds remaining window, to avoid deadlock when `window_bytes < max_chunk_bytes`. That chunk must be the last until credit is restored. |
| **Idle timeout** | If no data or ack frame is received within `idle_timeout` (default 30s), the stream is errored with TIMEOUT. |

---

## 8. Receiver-Side Delivery

The `StreamReceiverActor` delivers chunks to the target actor as regular
`TypedMessage`s. The stream subsystem declares its own `TypeTag`s using the
`make_subsystem_tag()` mechanism (extension range `0x80–0xFF`):

| TypeTag | When Delivered | Payload |
|---------|---------------|---------|
| `StreamChunkTag` (0x80) | Each data chunk arrives | Original chunk data with original `TypeTag` |
| `StreamOpenedTag` (0x81) | Stream session established | `StreamOpenedPayload` (stream_id, sender, config) |
| `StreamClosedTag` (0x82) | Stream gracefully closed | `StreamClosedPayload` (stream_id, reason, total_bytes) |
| `StreamErrorTag` (0x83) | Stream errored | `StreamErrorPayload` (stream_id, error_code, description) |

The receiving actor handles these like any other message. Chunks arrive with
the original user `TypeTag` preserved — the `StreamReceiverActor` wraps the
original payload but does not change its type.

```cpp
class DataConsumer : public EventBasedActor {
    Behavior make_behavior() override {
        return Behavior{
            // Handle stream data chunks (original TypeTag preserved)
            on<SensorReading>([](const TypedMessage& msg) {
                process_reading(msg);
            }),
            // Stream lifecycle
            on_stream_opened([](const StreamOpenedPayload& p) {
                log::info("stream {} opened from {}", p.stream_id, p.sender);
            }),
            on_stream_closed([](const StreamClosedPayload& p) {
                log::info("stream {} closed, {} bytes received",
                          p.stream_id, p.total_bytes);
            }),
            on_stream_error([](const StreamErrorPayload& p) {
                log::error("stream {} error: {}", p.stream_id, p.description);
            }),
        };
    }
};
```

---

## 9. Local Fast Path

When sender and receiver are on the same node, the stream frames bypass
serialization and transport entirely:

```text
Remote path:
  StreamSenderActor → encode → WireEnvelope → transport→try_send() → wire
                   → ActorSystem::deliver_remote() → decode → StreamReceiverActor

Local fast path:
  StreamSenderActor → try_push_batch() → StreamReceiverActor mailbox
```

`StreamAckFrame`s flow back through the `StreamSenderActor`'s mailbox in both
cases, preserving the credit-window backpressure mechanism.

---

## 10. Error Handling

| Scenario | Detected by | Behavior |
|----------|-------------|----------|
| Target unreachable at open | `ActorContext::open_stream()` | Returns `std::nullopt` |
| Target actor exits mid-stream | `StreamSenderActor` | Sends `StreamErrorFrame`, `is_open()` → false |
| Sender actor exits | `ActorSystem` on actor exit | All owned streams closed with `CANCELLED` |
| Idle timeout | `StreamSenderActor` timer | Sends `StreamErrorFrame(TIMEOUT)` |
| Window starvation timeout | `StreamSenderActor` timer | Same as idle timeout |
| Out-of-order sequence | `StreamReceiverActor` | Sends `StreamErrorFrame`, drops stream |
| Corrupt wire frame | `WireFrame::decode()` | Frame dropped; sender retransmits on timeout |

Cleanup is guaranteed: when a stream terminates, both stream actors
self-terminate after notifying their user actors. `write()` on a closed handle
is a no-op returning `false`.

---

## 11. Integration with Existing Subsystems

| Subsystem | Integration Point |
|-----------|------------------|
| **Mailbox / MultiLaneQueue** | Control frames use system lane; data frames use user lane. Window tracks target mailbox pressure. |
| **Supervision** | Stream actors are children of the user's actor. Supervisor strategy governs restart behavior. |
| **Tracing** | Stream `TraceContext` set at open, propagated to all frames and delivered chunks. |
| **Dead Letter Queue** | New `DeadLetterReason::StreamClosed` (39). Chunks after close/error, or to unknown targets, route to DLQ. |
| **Metrics** | 9 new metrics: open/close counters, byte counters, window/in-flight gauges, chunk counters. |
| **CLI** | `/stream list` (active streams table) and `/stream show <id>` (detailed state) via `InspectState`. |
| **Fault Injection** | New `FaultDomain::Stream` with 5 fault points (open, data, ack, close, error) supporting Fail/Drop/Delay actions. |
| **SpawnReceiver** | Remote `StreamReceiverActor` spawned via existing remote spawn infrastructure. |
| **Actor Lifecycle** | Stream actors follow the standard `LifecycleActor` state machine (Created → Starting → Running → Stopping → Stopped). |

---

## 12. Observability

### Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `hpactor_streams_opened_total` | Counter | Streams opened (sender side) |
| `hpactor_streams_accepted_total` | Counter | Streams accepted (receiver side) |
| `hpactor_streams_closed_total` | Counter | Streams closed, labeled by reason |
| `hpactor_stream_bytes_sent_total` | Counter | Total bytes sent |
| `hpactor_stream_bytes_received_total` | Counter | Total bytes received |
| `hpactor_stream_window_bytes` | Gauge | Current advertised receiver window |
| `hpactor_stream_bytes_in_flight` | Gauge | Current bytes in flight |
| `hpactor_stream_chunks_sent_total` | Counter | Total data frames sent |
| `hpactor_stream_chunks_received_total` | Counter | Total data frames received |

### CLI

```
/> /stream list
┌────────────┬──────────────────┬──────────────────┬───────────┬───────────────┐
│ Stream ID  │ Sender           │ Receiver         │ State     │ In Flight     │
├────────────┼──────────────────┼──────────────────┼───────────┼───────────────┤
│ 0x2a000001 │ telemetry-prod-1 │ aggregator-3     │ STREAMING │ 12.5 KiB      │
│ 0x2a000002 │ telemetry-prod-1 │ aggregator-3     │ STREAMING │ 0 B (paused)  │
│ 0x2a000003 │ sensor-gw-7      │ dlq-collector-1  │ CLOSING   │ 1.2 KiB       │
└────────────┴──────────────────┴──────────────────┴───────────┴───────────────┘

/> /stream show 0x2a000001
Stream ID:       0x2a000001
Sender:          telemetry-prod-1
Receiver:        aggregator-3
State:           STREAMING
Window:          48.0 KiB
Bytes In Flight: 12.5 KiB
Chunks Sent:     1,847
Chunks Acked:    1,842
Opened:          2026-06-25T14:32:01.123Z
Idle:            0.5s ago
Trace ID:        a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6
```

---

## 13. Configuration

```toml
[system.stream]
enabled = true
default_initial_window_bytes = 65536     # 64 KiB
default_max_chunk_bytes = 65536          # 64 KiB
default_send_buffer_bytes = 262144       # 256 KiB
default_idle_timeout_ms = 30000          # 30 seconds
default_max_in_flight_frames = 256
```

Per-stream overrides are set via `StreamConfig` at `open_stream()` time:

```cpp
auto cfg = StreamConfig{
    .initial_window_bytes = 128 * 1024,
    .idle_timeout = Duration::from_seconds(60),
};
auto handle = context()->open_stream(target, cfg);
```

---

## 14. Relationship to Other Messaging Features

| Feature | How Streaming Relates |
|---------|----------------------|
| **MSG-001 Delivery Semantics** | Streams use best-effort delivery per chunk. Reliable streaming (ACK/NACK per chunk) is deferred to MSG-008b. |
| **MSG-007 Batch Send/Receive** | Batch is one-shot bulk delivery; streaming is continuous session-oriented delivery. Both share the `WireEnvelope` migration and local fast-path pattern. |
| **Bounded Mailboxes** | Stream window integrates directly with mailbox watermarks — the receiver advertises less credit when the target mailbox is under pressure. |
| **Dead Letter Queue** | Chunks that arrive after stream close/error are dead-lettered; stream open requests to unknown actors produce DLQ records. |
| **Distributed Tracing** | A single `TraceContext` covers the entire stream lifecycle. All chunks inherit the stream's trace. |

---

## 15. Future Directions (Out of Scope for MSG-008)

- **MSG-008b: Reliable streaming** — per-chunk ACK/NACK, selective retransmit,
  sender retry buffer with TTL. Makes streams work over lossy transports.
- **Protocol negotiation** — feature flags in the handshake so peers can
  advertise stream support and negotiate window sizes.
- **Stream multiplexing** — multiple logical streams over one transport
  connection, reducing connection overhead for many concurrent streams.
- **Durable stream cursors** — persist stream position so a restarted receiver
  can resume from the last acked sequence.
- **Auto-batching** — transparently coalesce consecutive `send()` calls to the
  same target into a stream, without explicit `open_stream()`.
