# MSG-008b: Streaming Protocol Completion — Implementation Plan

> **Issue:** [#400](https://github.com/skg7on/HPActor/issues/400)
> **Date:** 2026-06-30
> **Status:** In Progress
> **Depends on:** #21 (original MSG-008), #365 (foundation merge)

**Goal:** Fix critical bugs and wire up the `StreamHandle` → `StreamSenderActor` bridge so streaming sessions work end-to-end for both local and cross-node delivery.

## Architecture Summary

The PR #365 architecture is sound — the gap is the wiring between the user-facing `StreamHandle` and the internal `StreamSenderActor`, plus several bugs in the existing code paths.

```
StreamHandle                   StreamSenderActor              StreamReceiverActor
    │                               │                               │
    │ write(msg) ──deliver_fn──►    │ enqueue_chunk()               │
    │                               │ send_pending_chunks()         │
    │                               │   ├─ local: try_deliver ──►  │ handle_stream_data()
    │                               │   └─ remote: WireFrame ──►   │ send_ack()
    │                               │                               │
    │ close() ────deliver_fn──►     │ handle from mailbox          │
    │ error() ────deliver_fn──►     │ send Close/Error frames      │
```

## Key Design Decisions

### 1. StreamHandle delivery callback

`StreamHandle` is a public header and cannot include `actor_system.hpp`. Following the codebase pattern used by `ReliableAckPort` and `BackpressureWirePort`, `StreamHandle` stores a C-style function pointer + `void*` context:

```cpp
using DeliverFn = bool (*)(void* ctx, ActorId target, TypedMessage msg);
```

`ActorSystem::open_stream()` binds `this` as the context and a static dispatch function that calls `try_deliver_local_fast`. This avoids heavy header dependencies while keeping the call path direct.

### 2. bytes_in_flight / window_bytes queries

These are observability helpers. For this phase, they remain returning 0 with a TODO. The values are available via CLI `/stream show` (future phase). The critical path is write/close/error.

### 3. Remote delivery in send_pending_chunks

The sender actor currently only uses `try_deliver_local_fast`. Adding a stored `is_local_` flag (set at construction) and a `TcpTransport*` reference for remote sends. For the local path, data frames go direct to the receiver's mailbox. For remote, they serialize to `WireFrame` and go through transport.

---

## Task Breakdown

### Phase 1: Critical Bug Fixes

| # | Task | Files | Priority |
|---|------|-------|----------|
| 1.1 | Add deliver callback to StreamHandle | `stream_handle.hpp` | Blocker |
| 1.2 | Implement write() via deliver callback | `stream_handle.hpp` | Blocker |
| 1.3 | Implement close() via deliver callback | `stream_handle.hpp` | Blocker |
| 1.4 | Implement error() via deliver callback | `stream_handle.hpp` | Blocker |
| 1.5 | Fix deliver_remote_stream_ack serialization | `actor_system.cpp` | Blocker |
| 1.6 | Add local/remote branch in send_pending_chunks | `stream_sender_actor.{hpp,cpp}` | Blocker |
| 1.7 | Wire idle timeout timer | `stream_sender_actor.{hpp,cpp}` | High |

### Phase 2: Protocol Completeness

| # | Task | Files | Priority |
|---|------|-------|----------|
| 2.1 | Deliver StreamOpenedTag for local targets | `actor_system.cpp` | Medium |
| 2.2 | Capture trace context in open_stream() | `actor_system.cpp` | Medium |
| 2.3 | Add DeadLetterReason::StreamClosed | `dead_letter_record.hpp` | Medium |

### Phase 3: Tests

| # | Task | Files | Priority |
|---|------|-------|----------|
| 3.1 | Stream frame roundtrip unit tests | `tests/unit/msg/test_stream_frames.cpp` (new) | High |
| 3.2 | Stream messaging integration tests | `tests/integration/actor/test_stream_messaging.cpp` (new) | High |

### Phase 4: Build Verification

| # | Task |
|---|------|
| 4.1 | Build all targets |
| 4.2 | Run focused test suites |
| 4.3 | Run full ctest |

---

## Detailed Implementation Notes

### 1.1–1.4: StreamHandle delivery callback

New constructor signature:
```cpp
StreamHandle(ActorId sender_actor_id, uint64_t stream_id,
             DeliverFn deliver_fn, void* deliver_ctx);
```

Static dispatch function for `ActorSystem::open_stream()`:
```cpp
static bool deliver_to_stream_sender(void* ctx, ActorId target, TypedMessage msg) {
    auto* sys = static_cast<ActorSystem*>(ctx);
    return sys->try_deliver_local_fast(target, std::move(msg)) != nullptr;
}
```

`write()` constructs a TypedMessage with the user's TypeTag + payload, sets sender address if available, and calls `deliver_fn_(deliver_ctx_, sender_actor_id_, msg)`.

`close()` constructs a TypedMessage with `stream::StreamCloseTag` and an empty payload (StreamSenderActor builds the StreamCloseFrame).

`error()` constructs a TypedMessage with `stream::StreamWireErrorTag` and the error code/description.

### 1.5: Ack serialization fix

Replace the raw struct copy in `deliver_remote_stream_ack`:
```cpp
// Before (broken):
auto ack_buf = StreamBuffer::from_data(reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));

// After (correct):
std::string serialized;
ack.SerializeToString(&serialized);
auto ack_buf = StreamBuffer::from_data(
    reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
```

### 1.6: Local/remote branch

`StreamSenderActor` receives an additional `bool is_local` flag at construction. `send_pending_chunks()` checks it:
- **Local:** Current path via `try_deliver_local_fast`
- **Remote:** Serialize `StreamDataFrame` to `WireFrame`, encode, send via `TcpTransport::try_send()`

Remote path also requires storing the target `ActorAddress` for transport routing.

### 1.7: Idle timeout

In `make_behavior()`, schedule initial idle timeout:
```cpp
auto handle = context()->schedule(config_.idle_timeout, 
    TypedMessage(stream::InternalTimeoutTag, StreamBuffer{}));
```

Reset the timer in `enqueue_chunk()` and each `handle_stream_*` method via `context()->cancel_schedule(handle)` + re-schedule.

A dedicated `InternalTimeoutTag` in the subsystem range triggers `on_idle_timeout()` in the behavior dispatch.

---

## Files Changed

| File | Change |
|------|--------|
| `include/hpactor/actor/stream_handle.hpp` | Add deliver callback, implement write/close/error |
| `include/hpactor/actor/stream_sender_actor.hpp` | Add is_local flag, ActorAddress, idle timer handle |
| `src/actor/stream_sender_actor.cpp` | Remote path, idle timeout scheduling |
| `src/actor/actor_system.cpp` | Fix ack serialization, deliver callbacks, trace context, StreamOpenedTag |
| `include/hpactor/msg/dead_letter_record.hpp` | Add StreamClosed reason |
| `tests/unit/msg/test_stream_frames.cpp` | **New:** frame roundtrip tests |
| `tests/integration/actor/test_stream_messaging.cpp` | **New:** integration tests |
| `tests/unit/msg/CMakeLists.txt` | Add test_stream_frames.cpp |
| `tests/integration/actor/CMakeLists.txt` | Add test_stream_messaging.cpp |

## Build Verification Sequence

1. `ninja -C build` — targeted build
2. `./build/tests/unit/actor/test_unit_actor --gtest_filter="*Stream*"` — stream unit tests
3. `./build/tests/unit/msg/test_unit_msg --gtest_filter="*StreamFrame*"` — frame tests
4. `./build/tests/integration/actor/test_integration_actor --gtest_filter="*StreamMessaging*"` — integration
5. `ctest --output-on-failure --parallel 8` — full suite, verify no regressions
