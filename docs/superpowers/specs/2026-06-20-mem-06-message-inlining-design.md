# MEM-006: Message Inlining for Tiny Payloads — Design Spec

**Date:** 2026-06-20
**Branch:** (future) `feature/mem-006-message-inlining`
**Issue:** #339 (Phase 2.3, P1)
**Parent Doc:** `docs/architecture/memory/memory-management-erlang-gap-analysis.md`

## 1. Motivation

### 1.1 Problem

Every message in HPActor, regardless of size, goes through the full allocation
path: admission check → region reserve → bump/freelist → header stamp → canary
stamp. For messages ≤ 32 bytes (ActorId-sized commands, simple signals, ping
messages), the 40-byte allocator overhead (32B `AllocHeader` + 8B
`CanaryFooter`) exceeds the payload.

At high throughput (e.g., `bench_saturate` sending 16B payloads at millions of
messages/second), this means:

- **125% overhead per tiny message** — 16B payload + 40B overhead = 56B block.
- **Allocator pressure** — millions of allocations/second hitting the
  `kMessage` region, CAS-based freelist, and telemetry sampling.
- **Cache pollution** — each allocation touches at least 3 cache lines (header,
  payload, footer).

### 1.2 Expected Impact

| Metric | Before | After | Delta |
|--------|--------|-------|-------|
| Allocations for messages ≤ 32B | 100% (every message) | 0% (inlined) | -100% for tiny msgs |
| Total allocations (message-heavy workload) | baseline | 60–80% of baseline | -20–40% |
| Throughput (bench_saturate tiny msgs) | baseline | +20–40% | Significant |
| Cache lines touched per tiny message | 3 (header+payload+footer) | 1 (envelope inline buffer) | -67% |

---

## 2. Design Goals

1. **Zero allocation for messages ≤ 32 bytes** — payload inlined directly in
   the mailbox envelope.
2. **Transparent to sender and receiver** — the API is unchanged; messages
   appear as `TypedMessage<T>` regardless of inlining.
3. **No per-message branch on send** — the decision to inline is made at
   compile time based on payload size.
4. **Works with `MultiLaneQueue`** — the existing lock-free multi-lane queue
   gains inline buffer support in its envelope type.
5. **Preserves all existing semantics** — delivery guarantees, tracing, DLQ,
   and backpressure are unchanged.

---

## 3. Design

### 3.1 Inline Envelope

The `MultiLaneQueue` envelope gains an inline buffer, stored in a union with
the external payload pointer:

```cpp
// include/hpactor/mailbox/multi_lane_queue.hpp

template <typename T>
class MultiLaneQueue {
    struct Envelope {
        // ... existing fields ...

        // Payload storage: inline or external
        union {
            void* external_payload;                     // Pointer to allocated message
            uint8_t inline_payload[kMaxInlinePayload];  // Inline buffer
        };

        enum Flags : uint8_t {
            kFlagExternal = 0x00,   // external_payload is valid
            kFlagInlined  = 0x01,   // inline_payload is valid
            // ... other flags ...
        };
        uint8_t flags;

        // Size of the payload (inline or external)
        uint16_t payload_size;

        // ... other fields (TypeTag, TraceContext, etc.) ...
    };
    static_assert(sizeof(Envelope) <= 128);  // cache-line aligned
};
```

**Constants:**

```cpp
// Maximum payload size for inlining
static constexpr size_t kMaxInlinePayload = 32;

// Envelope size budget: ensure envelopes stay within 2 cache lines (128B)
static_assert(sizeof(Envelope) <= 128, "Envelope must fit in 2 cache lines");
```

### 3.2 Compile-Time Dispatch

The decision to inline is determined by a `constexpr` template:

```cpp
// include/hpactor/types/typed_message.hpp

template <typename T>
inline constexpr bool kCanInlinePayload =
    sizeof(T) <= kMaxInlinePayload &&
    std::is_trivially_copyable_v<T>;

template <typename T>
struct TypedMessage {
    // ... existing ...
    static constexpr bool is_inlined = kCanInlinePayload<T>;
};
```

The sender path branches at compile time:

```cpp
// In ActorContext::send() or equivalent delivery path:
template <typename T>
void deliver_message(ActorAddress target, TypedMessage<T> msg) {
    if constexpr (TypedMessage<T>::is_inlined) {
        // Inline path: copy payload into envelope inline buffer
        auto* env = acquire_envelope();
        memcpy(env->inline_payload, &msg.payload, sizeof(T));
        env->flags |= Envelope::kFlagInlined;
        env->payload_size = sizeof(T);
    } else {
        // External path: allocate from kMessage region (unchanged)
        auto* buf = mem::allocate(RegionType::kMessage, sizeof(T), owner_id);
        memcpy(buf, &msg.payload, sizeof(T));
        auto* env = acquire_envelope();
        env->external_payload = buf;
        env->flags |= Envelope::kFlagExternal;
        env->payload_size = sizeof(T);
    }
    target_mailbox->enqueue(env);
}
```

### 3.3 Receiver Path

The receiver is unchanged — it accesses the payload through a uniform accessor:

```cpp
// In Envelope:
const void* payload_data() const {
    if (flags & kFlagInlined)
        return inline_payload;
    else
        return external_payload;
}

size_t payload_size() const {
    return payload_size_;
}
```

When the receiver processes the message, it deserializes from `payload_data()`
regardless of inlining. On dequeue, the envelope is freed to its own pool
(`ObjectPool<Envelope>`) — the inline buffer requires no separate deallocation.
For external payloads, the existing `mem::deallocate()` path is used.

### 3.4 Envelope Pool Integration

`MultiLaneQueue` envelopes are already managed via `ObjectPool<Envelope>` for
bounded admission. The inlining design maintains this:

```cpp
// Envelope lifecycle:
// 1. Acquire from pool (configurable capacity, e.g., 64K envelopes)
// 2. Fill: copy inline or allocate external
// 3. Enqueue to MultiLaneQueue
// 4. Dequeue: receiver processes
// 5. Release back to pool:
//    - Inline: just return to pool (no deallocation needed)
//    - External: deallocate external_payload, then return to pool
```

### 3.5 Tracing Integration

The `TraceContext` span propagation is unchanged — the trace context is part of
the envelope, not the payload. Inlined messages still carry trace spans.

### 3.6 DLQ Integration

When a message is dead-lettered, the DLQ stores a copy of the payload. For
inlined messages, the copy comes from the inline buffer:

```cpp
// DeadLetterRecord::from_envelope(envelope):
DeadLetterRecord record;
record.payload = std::vector<uint8_t>(
    static_cast<const uint8_t*>(envelope->payload_data()),
    static_cast<const uint8_t*>(envelope->payload_data()) + envelope->payload_size()
);
// Works identically for inline and external payloads
```

### 3.7 Size Limitations

| Payload Type | Typical Size | Inlined? |
|-------------|-------------|----------|
| `ActorId` | 8B | ✅ Yes |
| `MessageId` | 12B | ✅ Yes |
| `AlarmHandle` | 8B | ✅ Yes |
| Simple enum/signal | 4–16B | ✅ Yes |
| Bench saturate payload (16B) | 16B | ✅ Yes |
| Small protobuf message | 24–32B | ✅ Yes |
| Medium protobuf message | 33–64B | ❌ No (external) |
| Large payload | >64B | ❌ No (external) |

### 3.8 Fault Injection

| Site | Domain | Action | Purpose |
|------|--------|--------|---------|
| `hpactor.mailbox.inline_payload_corrupt` | Mailbox | Corrupt | Simulate inline payload corruption in transit |

---

## 4. Implementation Plan

### 4.1 TDDFlow Sequence

| Step | Test | Implementation |
|------|------|----------------|
| 1 | `InlineEnvelopeConstruction` — envelope with inline payload stores data correctly | Envelope union, `kFlagInlined`, inline buffer |
| 2 | `InlineEnvelopePayloadAccess` — `payload_data()` returns correct pointer for inline/external | `payload_data()`, `payload_size()` accessors |
| 3 | `InlineMessageDelivery` — actor sends 16B message, receiver gets correct payload | `ActorContext::send()` compile-time inline path |
| 4 | `InlineMessageNoAllocation` — verify no `mem::allocate()` call for inline message | Zero-allocation verification |
| 5 | `InlineMessageEnvelopeRecycle` — envelope returns to pool, inline buffer has no leak | Pool acquire/release cycle |
| 6 | `InlineMessageDLQ` — dead-lettered inline message preserves payload | DLQ record from inline envelope |
| 7 | `InlineMessageTraceContext` — trace span survives inline delivery | Trace propagation test |

### 4.2 Files Changed

| File | Change |
|------|--------|
| `include/hpactor/mailbox/multi_lane_queue.hpp` | Add inline buffer union to Envelope, `kFlagInlined`, `payload_data()` accessor |
| `include/hpactor/types/typed_message.hpp` | Add `kCanInlinePayload<T>` constexpr, `is_inlined` |
| `src/actor/actor_context.cpp` | Add compile-time inline delivery path in `send()` |
| `src/mailbox/multi_lane_queue.cpp` | Envelope release: handle inline vs external deallocation |
| `src/mailbox/dead_letter_queue.cpp` | DLQ copy from `payload_data()` (no-change, already unified path) |
| `tests/unit/mailbox/test_message_inlining.cpp` | **New file** — 7 test cases |
| `tests/unit/mailbox/CMakeLists.txt` | Add new test target |

---

## 5. Testing Strategy

### 5.1 Unit Tests

| Test | What It Validates |
|------|-------------------|
| `InlineEnvelopeConstruction` | Inline buffer stores 32B correctly, flag set |
| `InlineEnvelopePayloadAccess` | `payload_data()` returns inline buffer for inlined, external pointer for external |
| `InlineMessageDelivery` | End-to-end: send → enqueue → dequeue → receive for 16B message |
| `InlineMessageNoAllocation` | Track `mem::allocate()` calls; verify zero for inline messages |
| `InlineMessageEnvelopeRecycle` | Envelope released to pool and re-acquired; inline buffer intact |
| `InlineMessageDLQ` | Dead-letter an inline message; verify payload preserved |
| `InlineMessageTraceContext` | Trace span propagated through inline delivery |

### 5.2 Integration Tests

| Test | What It Validates |
|------|-------------------|
| `MemInlineMessageStress` | 1M inline messages through 4-thread system; no leaks, no corruption |
| `MemInlineMixedSizes` | Mix of inline (≤32B) and external (>32B) messages; all delivered correctly |

### 5.3 Performance Tests

| Test | What It Measures |
|------|-----------------|
| `PerfInlineMessageThroughput` | Compare throughput: inline vs external for 16B messages at 1M ops |
| `PerfInlineAllocReduction` | Allocations/second with and without inlining under bench_saturate workload |

---

## 6. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| Envelope size exceeds 2 cache lines → cache miss on queue operations | Low | `static_assert(sizeof(Envelope) <= 128)`. Current envelope ~96B; 32B inline buffer brings to 128B exactly. |
| `memcpy` into inline buffer for every message (even tiny) vs pointer assignment for external | None | Copying 32B is < 1ns and cheaper than a pointer indirection + allocation + header stamp. Cache-local. |
| Inline buffer reduces envelope pool capacity (same memory, fewer envelopes) | Low | 32B × 64K pool = 2MB extra. Configurable. Default 64K pool is generous. |
| Protobuf `TypedMessage` with inlined payload doesn't fit serialization path | Low | Inlining is a mailbox-layer optimization; protobuf wire format is unaffected. Serializer reads from `payload_data()`. |
| Envelope alignment: `inline_payload` is `uint8_t[32]` with alignment 1 | Low | The types that can be inlined are constrained to `trivially_copyable`; `memcpy` handles alignment. If alignment matters for performance, `alignas(8)` on the buffer. |

---

## 7. Acceptance Criteria

1. Messages ≤ 32B (`ActorId`, `MessageId`, small signals) are inlined with zero
   allocations.
2. Messages > 32B continue to use external allocation (unchanged behavior).
3. All existing mailbox tests (23 files) pass — no regression.
4. Bench saturate `quick-saturate` shows ≥ 20% throughput improvement for 16B
   payload.
5. `test_message_inlining` — all 7 test cases pass.
6. DLQ correctly preserves inline message payloads.
7. Tracing survives inline delivery without data loss.
8. TSan-clean and ASan-clean.
