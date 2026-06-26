# MSG-007: Batch Send & Receive Protocol — Design Spec

**Issue:** [#20](https://github.com/skg7on/HPActor/issues/20)  
**Date:** 2026-06-24  
**Status:** Approved  
**Subsystem:** Messaging  
**Priority:** P1  
**Backlog ref:** `docs/architecture/production/architecture-requirement-backlog.md#L82`

## 1. Motivation

High-throughput remote workloads incur per-message overhead at multiple layers:
separate `WireFrame` encoding, individual `transport->try_send()` calls, and
single-message CAS operations in the target mailbox. When an actor sends N
messages to the same remote target in rapid succession, those N messages should
travel as one batch frame on the wire and land as one batch enqueue in the
receiver's mailbox.

The local path already supports batch enqueue via
`MPSCActorMailbox::try_push_batch()`. The remote path does not — this spec
adds it.

## 2. Scope

**In scope:**
- Batch wire format: compact `BatchMsgFrame` protobuf with shared sender/receiver
- Sender API: `ActorContext::send_batch()` → `ActorProxy::try_send_batch()`
- Transport API: `Transport::try_send_batch()`
- Receiver dispatch: decode batch → `try_push_batch()` on target mailbox
- Per-message trace context propagation within the batch
- Per-message ACK/NACK flags within the batch

**Out of scope (follow-up issues):**
- Streaming sessions with flow-control windows (MSG-004 streaming half)
- `DeliveryPipeline::try_deliver_batch()` — full per-message pipeline gating for batches
- Auto-batching in the transport layer (opportunistic buffering of individual sends)
- Protocol negotiation / feature flags (assumes both peers speak the batch format)

## 3. Wire Protocol

### 3.1 Magic Header

Unchanged. All frames use `"HPAC"` (`0x43415048` little-endian). The protobuf
`WireEnvelope` oneof discriminator distinguishes batch from single-message, ACK,
and NACK frames.

### 3.2 Wire Format

```
[4 bytes: magic "HPAC" 0x43415048 LE]
[4 bytes: remaining_length in network byte order]
[N bytes: protobuf-serialized WireEnvelope]
```

### 3.3 Protobuf Changes (`protos/hpactor/frame.proto`)

```protobuf
message BatchEntry {
  uint32 type_tag = 1;
  uint64 message_id = 2;
  uint32 flags = 3;
  bytes payload = 4;
  PbTraceContext trace_context = 5;  // optional per-message trace
}

message BatchMsgFrame {
  hpactor.PbActorAddress sender = 1;
  hpactor.PbActorAddress receiver = 2;
  repeated BatchEntry entries = 3;
}

// Extend existing WireEnvelope:
message WireEnvelope {
  oneof payload {
    ActorMsgFrame data_frame = 1;   // existing single message
    AckFrame ack_frame = 2;         // existing ACK
    NackFrame nack_frame = 3;       // existing NACK
    BatchMsgFrame batch_frame = 4;  // NEW
  }
}
```

`WireEnvelope` already exists in `frame.proto` and wraps the existing
`ActorMsgFrame`, `AckFrame`, and `NackFrame`. Adding `batch_frame` as field 4
is backward-compatible — existing peers decoding an envelope with field 4 will
ignore it as an unknown field (proto3 behavior).

### 3.4 C++ Frame Types — WireFrame Migration to WireEnvelope

The existing `WireFrame` struct wraps `ActorMsgFrame` directly, but the proto
already defines `WireEnvelope` as the intended wire wrapper (with `oneof` for
`data_frame | ack_frame | nack_frame`). This spec completes that migration:
`WireFrame` is changed to hold a `WireEnvelope` instead of a raw `ActorMsgFrame`.

**Before (current code):**
```cpp
struct WireFrame {
    uint32_t magic_hdr;
    size_t length;
    ::hpactor::net::ActorMsgFrame pb_frame;  // direct
};
```

**After (this spec):**
```cpp
struct WireFrame {
    static constexpr uint32_t MagicHeader = 0x43415048;

    uint32_t magic_hdr = MagicHeader;
    size_t length;
    ::hpactor::net::WireEnvelope pb_envelope;  // wraps oneof

    StreamBuffer encode() const;           // serializes pb_envelope
    static WireFrame decode(const StreamBuffer& data);
    static WireFrame decode(std::span<const uint8_t> data);

    // Convenience factories
    static WireFrame from_data(::hpactor::net::ActorMsgFrame msg);
    static WireFrame from_batch(::hpactor::net::BatchMsgFrame batch);
    static WireFrame from_ack(::hpactor::net::AckFrame ack);
    static WireFrame from_nack(::hpactor::net::NackFrame nack);

    // Discriminator for receive-side routing
    enum class PayloadType { Data, Ack, Nack, Batch, Unknown };
    PayloadType payload_type() const;
};
```

**Migration impact:**
- `encode()` now serializes `pb_envelope` (one extra protobuf wrapper layer, ~2-4 bytes on wire).
- `decode()` parses `WireEnvelope` and dispatch is gated on `payload_type()`.
- Existing `pb_frame.mutable_sender()` call sites change to
  `pb_envelope.mutable_data_frame()->mutable_sender()`.
- This is a **wire-incompatible change**: old peers sending raw `ActorMsgFrame`
  will fail to parse as `WireEnvelope`. Since `WireEnvelope` migration was
  already planned (see proto comments) and batch is P1, this is the right
  time to make the switch.

## 4. Sender-Side API

### 4.1 ActorContext

```cpp
/// \brief Send a batch of messages to a single target actor.
///
/// All messages in \p msgs MUST target the same actor. If the target is
/// local, the batch is enqueued directly via \c try_push_batch() on the
/// target mailbox, bypassing serialization and the delivery pipeline.
/// If the target is remote, the batch is encoded as a single
/// \c BatchMsgFrame and sent over the transport.
///
/// \param[in] target Destination actor ID.
/// \param[in] msgs   Messages to batch-send (moved into the call).
/// \return \c DeliveryResult describing the overall outcome.
mailbox::DeliveryResult send_batch(ActorId target,
                                    std::vector<TypedMessage> msgs);
```

### 4.2 ActorProxy

```cpp
/// \brief Batch-send messages to a remote actor via the transport.
///
/// Constructs a \c BatchMsgFrame with shared sender/receiver, encodes it
/// into a \c WireEnvelope, and dispatches via \c try_send_batch().
///
/// \param[in] target Destination actor address.
/// \param[in] msgs   Messages to send (moved into the call).
/// \param[in] options Delivery options.
/// \return \c DeliveryResult.
mailbox::DeliveryResult try_send_batch(const ActorAddress& target,
                                        std::vector<TypedMessage> msgs,
                                        mailbox::DeliveryOptions options = {});
```

Implementation mirrors `try_send()` but:
1. Iterates `msgs` to build `BatchMsgFrame.entries[]`
2. Wraps in `WireEnvelope` with `batch_frame` oneof field set
3. Calls `transport_->try_send_batch(target, envelope.encode())`

### 4.3 Transport

```cpp
/// \brief Send a pre-encoded batch frame to a remote node.
///
/// \param[in] target  Destination actor address.
/// \param[in] encoded Encoded \c WireEnvelope containing a \c batch_frame.
/// \return \c TransportSendResult.
virtual TransportSendResult
try_send_batch(const ActorAddress& target, const StreamBuffer& encoded) = 0;
```

Default implementation in `Transport` base class falls back to decoding the
batch envelope and calling individual `try_send()` for each entry — guaranteeing
forward compatibility with transport backends that don't natively optimize
batch.

`TcpTransport` overrides to send the pre-encoded buffer directly (same path
as single-frame `try_send`, just accepting a different upstream frame type).

### 4.4 Local Fast Path

When `send_batch()` detects the target is local (same endpoint, same node):

```
context()->send_batch(local_id, msgs)
    → mailbox = get_mailbox(local_id)
    → mailbox->try_push_batch(msgs.begin(), msgs.end(), meta)
```

No serialization, no transport, no `DeliveryPipeline`. The existing
`try_push_batch()` handles single-reservation admission and chained-insert
edge-triggered wakeup. This is the same optimization `try_deliver_local_fast()`
provides for single messages.

## 5. Receiver-Side Dispatch

### 5.1 Frame Routing

`ActorSystem::deliver_remote()` uses `WireFrame::payload_type()` to route:

```cpp
void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    switch (frame.payload_type()) {
    case net::WireFrame::PayloadType::Ack:
        // Existing ACK dispatch...
        return;
    case net::WireFrame::PayloadType::Nack:
        // Existing NACK dispatch...
        return;
    case net::WireFrame::PayloadType::Batch:
        deliver_remote_batch(frame);
        return;
    case net::WireFrame::PayloadType::Data:
    default:
        break;  // Fall through to existing single-message path
    }
    // Existing single-message dispatch unchanged...
}
```

### 5.2 Batch Delivery

```cpp
void ActorSystem::deliver_remote_batch(const net::WireFrame& frame) {
    const auto& batch = frame.pb_envelope.batch_frame();
    ActorId receiver_id = net::from_proto(batch.receiver()).id;
    ActorAddress sender_addr = net::from_proto(batch.sender());

    std::vector<TypedMessage> msgs;
    msgs.reserve(batch.entries_size());

    for (const auto& entry : batch.entries()) {
        TypedMessage msg(static_cast<TypeTag>(entry.type_tag()),
                         StreamBuffer(entry.payload().begin(),
                                      entry.payload().end()));
        msg.set_sender_address(sender_addr);
        msg.set_message_id(entry.message_id());

        if (entry.has_trace_context()) {
            auto parsed = net::trace_context_from_proto(
                entry.trace_context(), tracing_config_.max_tracestate_len);
            if (parsed.has_value()) msg.set_trace_context(parsed.value());
        }
        if (entry.flags() & net::WireFrame::AckRequested) {
            msg.set_ack_requested(true);
        }
        msgs.push_back(std::move(msg));
    }

    auto* mailbox = get_mailbox(receiver_id);
    if (!mailbox) {
        // Target actor not found — dead-letter each message
        for (auto& msg : msgs) {
            DeadLetterRecord dl;
            dl.reason = DeadLetterReason::ActorNotFound;
            dl.source = DeadLetterSource::RemoteDelivery;
            dl.sender = sender_addr;
            dl.target = {receiver_id, batch.receiver().endpoint()};
            dl.type_tag = msg.type_id();
            dl.payload_sample = msg.payload();
            dead_letter(std::move(dl));
        }
        return;
    }

    MailboxEnvelopeMeta meta;
    meta.sender = sender_addr;

    mailbox->try_push_batch(msgs.begin(), msgs.end(), meta);
}
```

**Key properties:**
- Batch dispatch **bypasses** the `DeliveryPipeline` (dedup, circuit breaker, TTL
  per-message). It uses the fast path via `try_push_batch()`. The mailbox's
  existing bounded-capacity admission (via `try_push_batch`'s single
  reservation) still applies.
- Per-message trace context and ACK/NACK flags are preserved from the wire.
- If the target actor doesn't exist, each message in the batch is individually
  dead-lettered with the `ActorNotFound` reason.

## 6. Partial Failure Handling

`try_push_batch()` already handles partial failure: if the single batch
reservation fails (mailbox full), it falls back to individual `try_push()` calls
for each message. Each individual call can independently succeed or be rejected.

The batch envelope does **not** carry a result vector back to the sender. The
sender gets a single `DeliveryResult` indicating whether the batch frame was
accepted for delivery. Individual message rejection within the batch is
observable on the receiver side via dead-letter records, metrics, and DLQ
entries.

Full per-message delivery-result reporting for batches (NACK per entry) is
deferred to a follow-up that also adds `DeliveryPipeline::try_deliver_batch()`.

## 7. Trace Propagation

Each `BatchEntry` carries an optional `PbTraceContext`. When present, the trace
context is attached to the corresponding `TypedMessage`. When absent, the
message has no trace context.

This supports two models:
- **Per-message trace:** Each entry has its own span context (fine-grained).
- **Batch-root trace:** Callers set the same trace context on all entries,
  effectively treating the batch as a single traced operation.

No batch-level trace context is added to `BatchMsgFrame` — the per-entry field
covers both models without duplication.

## 8. Observability

New metric events (emitted by `ActorSystem::deliver_remote_batch()`):

| Metric | Type | Description |
|--------|------|-------------|
| `hpactor_batch_frames_received_total` | Counter | Number of batch frames received |
| `hpactor_batch_messages_received_total` | Counter | Total messages delivered via batch |
| `hpactor_batch_frames_sent_total` | Counter | Number of batch frames sent |
| `hpactor_batch_messages_sent_total` | Counter | Total messages sent via batch |

CLI: Extend `/system stats` to show batch counters.

## 9. Testing Strategy

### Unit Tests

| Test | What it validates |
|------|-------------------|
| `BatchMsgFrame roundtrip` | Encode → decode preserves all fields |
| `WireEnvelope batch discrimination` | Envelope with `batch_frame` oneof decodes correctly |
| `BatchEntry with trace` | Per-entry trace context survives roundtrip |
| `BatchEntry with flags` | ACK flag preserved through encode/decode |
| `Empty batch` | Zero entries encodes/decodes without crash |
| `Single-entry batch` | Degenerate batch (N=1) works like single message |
| `WireFrame batch encode/decode` | Full frame encode → decode roundtrip with batch payload |

### Integration Tests

| Test | What it validates |
|------|-------------------|
| `send_batch local` | Batch to local target uses fast path via `try_push_batch` |
| `send_batch remote` | Batch to remote target encodes + sends via transport |
| `deliver_remote_batch dispatch` | Receiver unpacks batch → `try_push_batch` on mailbox |
| `deliver_remote_batch actor not found` | Missing target → dead-letters each message |
| `batch with mixed trace` | Some entries with trace, some without |
| `batch metrics emitted` | Counters increment correctly |

## 10. Files Changed

| File | Change |
|------|--------|
| `protos/hpactor/frame.proto` | Add `BatchEntry`, `BatchMsgFrame`; extend `WireEnvelope` |
| `include/hpactor/msg/frame.hpp` | Migrate `WireFrame` to `WireEnvelope`; add `from_batch()`, `payload_type()` |
| `include/hpactor/actor/actor_context.hpp` | Add `send_batch()` declaration |
| `include/hpactor/ref/actor_proxy.hpp` | Add `try_send_batch()` declaration |
| `include/hpactor/net/transport.hpp` | Add `try_send_batch()` virtual method |
| `src/ref/actor_proxy.cpp` | Implement `try_send_batch()` |
| `src/actor/actor_context.cpp` | Implement `send_batch()` |
| `src/actor/actor_system.cpp` | Add `deliver_remote_batch()`, wire into `deliver_remote()` |
| `src/net/tcp_transport.cpp` | Implement `try_send_batch()` override |
| `src/net/frame.cpp` | Add `BatchWireFrame::encode()/decode()` |
| `src/metrics/` | Add batch metric events |
| `tests/unit/net/test_frame.cpp` | Add batch frame unit tests |
| `tests/integration/actor/test_batch_messaging.cpp` | New: integration tests |
