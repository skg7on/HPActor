# ActorSystem Lifetime and Ownership Inventory — Phase 4 Baseline

**Last updated:** 2026-06-30
**Purpose:** Record every frame/stream owner, callback capture, and outlives edge before Phase 4 extraction.

## 1. Encoded-Frame Producers and Frame-Handler Byte Shapes

### Producers
| Component | File | Produces |
|-----------|------|----------|
| `WireFrame::encode()` | `src/msg/frame.cpp:26` | 8-byte HPAC header + network-order length + protobuf payload |
| `WireFrameConnection::send()` | `src/net/wireframe_connection.cpp` | Writes raw bytes from write_buffer_ to fd |
| `TlsConnection::send()` | `src/net/tls_connection.cpp` | Encrypts frame data, wraps in TLS record |

### Frame Handler Byte Shapes
| Path | What the handler receives |
|------|--------------------------|
| Plain (WireFrameConnection) | Protobuf payload ONLY (HPAC header stripped at `wireframe_connection.cpp:250`) |
| TLS (TlsConnection) | Decrypted plaintext (HPAC header + protobuf, NOT stripped) |

**Finding:** Plain and TLS paths deliver different byte shapes to `frame_handler_`. TLS preserves the HPAC header; plain strips it. This means `ConnectionPool::on_frame_received()` calls `WireFrame::decode()` on both — but plain data lacks the header that decode expects. The header is stripped before delivery, then decode re-parses from position 0 expecting "HPAC" magic but getting protobuf bytes instead.

## 2. `WireFrame::decode()` Callers

| Caller | File | Line |
|--------|------|------|
| `ConnectionPool::on_frame_received()` | `src/net/connection_pool.cpp` | ~200 |
| Direct transport handler users (tests, apps) | various | — |

## 3. Payload Oneof/Flag Classifiers

### In ConnectionPool (`src/net/connection_pool.cpp:199-265`)
- `payload_type() == Data` → check `RpcResponse` flag → spawn_handler_ or rpc_handler_
- Everything else → `actor_message_handler_`

### In ActorSystem (`src/actor/actor_system.cpp:908-981`)
- Switch on `payload_type()` for StreamOpen/Data/Ack/Close/Error
- For Data: check `AckRequested` flag first (bit 5) → ACK handler
- Then check `AckResponse` flag (bit 6) → NACK handler
- Then check TypeTag for BackpressureSignalTag
- Otherwise: ordinary data delivery

**Finding:** `AckRequested` (bit 5) is checked FIRST — ordinary reliable data messages with only `AckRequested` set are consumed as ACK responses, never reaching their actor.

## 4. RPC, Reliable, Backpressure, Batch, and Stream Handlers

| Handler | Location | Owner |
|---------|----------|-------|
| RPC response | `ConnectionPool::on_frame_received()` | ConnectionPool |
| Spawn response | `ConnectionPool::on_frame_received()` | ConnectionPool |
| Reliable ACK | `ActorSystem::deliver_remote()` → `impl_->messaging_->on_reliable_ack()` | MessagingRuntime |
| Reliable NACK | `ActorSystem::deliver_remote()` → `impl_->messaging_->on_reliable_nack()` | MessagingRuntime |
| Backpressure | `ActorSystem::deliver_remote()` → `impl_->messaging_->backpressure().handle_remote_signal()` | MessagingRuntime |
| Batch | NOT IMPLEMENTED (falls through to data_frame) | — |
| Dedicated ACK oneof | NOT DISPATCHED (falls through to data_frame) | — |
| Dedicated NACK oneof | NOT DISPATCHED (falls through to data_frame) | — |
| Stream open/data/ack/close/error | `ActorSystem::deliver_remote_stream_*()` | ActorSystem facade |

## 5. Stream Map/Counter Access and Caller Threads

### Current State
| State | Location | Type |
|-------|----------|------|
| `streams.registry` | `src/runtime/actor_system_impl.hpp:122` | `StreamRegistry` (mutex + `unordered_map<uint64_t, ActorId>`) |
| `streams.counter` | `src/runtime/actor_system_impl.hpp:123` | `std::atomic<uint64_t>` |

**Finding:** StreamRegistry already has a mutex and is in `ActorSystem::Impl::StreamRuntimeState` (not directly in ActorSystem). But it keys by `uint64_t stream_id` only — no peer qualification. Two peers can collide.

### Thread Access
- **Event-loop thread:** `deliver_remote()` → `deliver_remote_stream_*()` → registry lookup/delivery/take
- **Actor threads:** `open_stream()` → `register_stream_sender/receiver()`
- **CLI thread:** `stream/list` (stub, empty table)
- **External threads:** None currently (public API returns stubs)

## 6. Transport Callback Captures and Setter Propagation

| Callback | Set By | Captures |
|----------|--------|----------|
| `actor_message_handler_` | `TcpTransport::set_actor_message_handler()` | `[this]` → ActorSystem* |
| `rpc_handler_` | `TcpTransport::set_rpc_handler()` | std::function, no facade capture |
| `frame_handler_` (per connection) | `TcpTransport::connect()/handle_accept()` | `[pool]` → shared_ptr<ConnectionPool> |
| `error_handler_` (per connection) | `TcpTransport::connect()/handle_accept()` | `[pool]` → shared_ptr<ConnectionPool> |

**Finding:** The `actor_message_handler_` callback in TcpTransport captures `ActorSystem*` via `[this]` in the constructor (actor_system.cpp:317-320).

## 7. Router/Stream/Messaging/RPC/Transport Outlives Edges

```
ActorSystem (facade)
  └── Impl
        ├── core (endpoint, etc.)
        ├── actor (ActorRuntime — Phase 2)
        ├── messaging (MessagingRuntime — Phase 3)
        │     ├── DeadLetterQueue
        │     ├── DedupCache
        │     ├── DeliveryPipeline
        │     ├── LocalDeliveryEngine
        │     ├── BackpressureCoordinator
        │     └── OutboundDeliveryTracker
        ├── streams (StreamRuntimeState)
        │     ├── StreamRegistry (senders_/receivers_ maps + mutex)
        │     └── atomic<uint64_t> counter
        ├── network (NetworkRuntimeState)
        │     ├── event_loop
        │     ├── transport (TcpTransport)
        │     └── discovery
        ├── operations (OperationsRuntimeState)
        └── cluster (ClusterRuntimeState)
```

**Destruction order (current):** ActorSystem destructor clears in declaration order (impl_ members destroyed bottom-up). No explicit ingress-disable-before-router-destroy ordering.

## 8. Shutdown Order

Current flow in `ActorSystem::shutdown()` (simplified):
1. Mark readiness false
2. Stop accepting new actors
3. Drain user actors
4. Drain system actors
5. Stop scheduler
6. Stop event loop / network thread
7. Destructor: ~Impl() cleans up in reverse declaration order

**Finding:** No explicit step to disable ingress callbacks before joining network thread. Router destruction happens in Impl destructor, after network thread join (network state declared before any new router state).

## 9. Known Gaps (Phase 4 Targets)

1. Plain/TLS frame handler bytes not canonical (header stripped on plain path)
2. No `try_decode()` — all failures return empty WireFrame
3. Recursive `handle_read()` in WireFrameConnection for magic resync
4. No inbound frame size limit before allocation
5. Two classification sites (ConnectionPool + ActorSystem)
6. `AckRequested` checked before `AckResponse` — consumes ordinary reliable data
7. Batch frames not dispatched
8. Dedicated ACK/NACK oneofs not dispatched
9. Stream registry keys by stream_id only (no peer)
10. Stream close/error use wrong TypeTags (StreamClosedTag vs StreamCloseTag)
11. Stream ACK handler copies C++ object memory instead of using protobuf serialization
12. `actor_message_handler_` captures ActorSystem*
