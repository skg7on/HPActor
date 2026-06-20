# MEM-006: Message Inlining — Implementation Plan

**Date:** 2026-06-20
**Design Spec:** `docs/superpowers/specs/2026-06-20-mem-06-message-inlining-design.md`
**Issue:** #339 (Phase 2.3, P1)
**Depends on:** None (standalone — mailbox-layer optimization)

## Phase Overview

| Phase | Name | Tests | Files Changed |
|-------|------|-------|---------------|
| 1 | `Envelope` inline buffer + `kFlagInlined` + `payload_data()` | 3 | 1 |
| 2 | `kCanInlinePayload<T>` constexpr | 2 | 1 |
| 3 | Compile-time inline delivery path in `ActorContext::send()` | 2 | 2 |
| 4 | Envelope release: inline vs external deallocation | 2 | 1 |
| 5 | DLQ preserves inline payloads | 2 | 1 |
| 6 | Tracing survives inline delivery | 1 | 1 |
| 7 | Integration stress: 1M mixed-size messages | 1 | — |

---

## Phase 1: Envelope Inline Buffer + Accessors

**Goal:** Add a 32-byte inline buffer to `MultiLaneQueue::Envelope`, unioned
with the existing external payload pointer. Add a `kFlagInlined` flag and
`payload_data()`/`payload_size()` accessors.

### RED — Write failing tests

File: `tests/unit/mailbox/test_message_inlining.cpp` (new)

```cpp
#include <gtest/gtest.h>
#include "hpactor/mailbox/multi_lane_queue.hpp"

using namespace hpactor;

TEST(MessageInlining, EnvelopeInlineBufferExists) {
    MultiLaneQueue<int>::Envelope env;
    EXPECT_LE(sizeof(env), 128u) << "Envelope must fit in 2 cache lines";
    // Inline buffer is in union — accessible via payload_data()
}

TEST(MessageInlining, PayloadDataReturnsInlineBuffer) {
    MultiLaneQueue<int>::Envelope env;
    uint8_t test_data[16] = {1, 2, 3, 4};
    memcpy(env.inline_payload, test_data, 16);
    env.flags |= Envelope::kFlagInlined;
    env.payload_size = 16;

    const void* data = env.payload_data();
    EXPECT_EQ(memcmp(data, test_data, 16), 0);
    EXPECT_EQ(env.payload_size, 16u);
}

TEST(MessageInlining, PayloadDataReturnsExternalPointer) {
    MultiLaneQueue<int>::Envelope env;
    uint8_t heap_data[64] = {0x42};
    env.external_payload = heap_data;
    env.flags = 0;  // kFlagExternal (default)
    env.payload_size = 64;

    EXPECT_EQ(env.payload_data(), heap_data);
}
```

### GREEN — Implement

1. Add to `MultiLaneQueue::Envelope` in `include/hpactor/mailbox/multi_lane_queue.hpp`:
   ```cpp
   static constexpr size_t kMaxInlinePayload = 32;

   struct Envelope {
       // ... existing fields ...

       union {
           void* external_payload;
           uint8_t inline_payload[kMaxInlinePayload];
       };

       enum Flags : uint8_t {
           kFlagExternal = 0x00,
           kFlagInlined  = 0x01,
           // ... existing flags ...
       };
       uint8_t flags;
       uint16_t payload_size;

       const void* payload_data() const {
           if (flags & kFlagInlined) return inline_payload;
           return external_payload;
       }
   };
   static_assert(sizeof(Envelope) <= 128);
   ```

### Verification:
```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*MessageInlining*"
```

---

## Phase 2: `kCanInlinePayload<T>` Constexpr

**Goal:** Compile-time trait to decide if a message type qualifies for inlining.

### RED — Write failing tests

```cpp
#include "hpactor/types/typed_message.hpp"

struct SmallPod { uint64_t a; uint32_t b; };  // 12 bytes
static_assert(sizeof(SmallPod) <= 32);

struct LargePod { uint8_t data[64]; };
static_assert(sizeof(LargePod) > 32);

struct NonTrivial {
    std::string s;  // not trivially copyable
};

TEST(MessageInlining, SmallTriviallyCopyableTypeCanInline) {
    EXPECT_TRUE((kCanInlinePayload<SmallPod>));
}

TEST(MessageInlining, LargeTypeCannotInline) {
    EXPECT_FALSE((kCanInlinePayload<LargePod>));
}

TEST(MessageInlining, NonTrivialTypeCannotInline) {
    EXPECT_FALSE((kCanInlinePayload<NonTrivial>));
}
```

### GREEN — Implement

In `include/hpactor/types/typed_message.hpp`:
```cpp
static constexpr size_t kMaxInlinePayload = 32;

template <typename T>
inline constexpr bool kCanInlinePayload =
    sizeof(T) <= kMaxInlinePayload &&
    std::is_trivially_copyable_v<T>;

template <typename T>
struct TypedMessage {
    static constexpr bool is_inlined = kCanInlinePayload<T>;
    T payload;
};
```

### Verification:
```bash
ninja -C build test_unit_mailbox test_unit_types
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*MessageInlining*"
```

---

## Phase 3: Compile-Time Inline Delivery Path

**Goal:** `ActorContext::send()` copies the payload into the envelope's inline
buffer when `T::is_inlined` is true, bypassing `mem::allocate()` entirely.

### RED — Write failing tests

```cpp
TEST(MessageInlining, InlineDeliverySkipsAllocation) {
    // Track allocations
    size_t allocs_before = MemoryRegionRegistry::instance()
        .snapshot(RegionType::kMessage).alloc_count;

    // Send 100 small messages
    ActorId receiver = spawn<TestReceiver>();
    for (int i = 0; i < 100; ++i) {
        TypedMessage<SmallPod> msg;
        msg.payload.a = i;
        msg.payload.b = i * 2;
        context()->send(receiver, msg);
    }

    size_t allocs_after = MemoryRegionRegistry::instance()
        .snapshot(RegionType::kMessage).alloc_count;

    // No new allocations for kMessage region (all inlined)
    EXPECT_EQ(allocs_after, allocs_before);
}

TEST(MessageInlining, ExternalDeliveryStillAllocates) {
    size_t allocs_before = MemoryRegionRegistry::instance()
        .snapshot(RegionType::kMessage).alloc_count;

    TypedMessage<LargePod> msg;
    context()->send(target, msg);

    size_t allocs_after = MemoryRegionRegistry::instance()
        .snapshot(RegionType::kMessage).alloc_count;

    EXPECT_GT(allocs_after, allocs_before);  // external allocation occurred
}
```

### GREEN — Implement

In `src/actor/actor_context.cpp` (or equivalent delivery path):
```cpp
template <typename T>
void deliver_message(ActorAddress target, TypedMessage<T> msg, ActorId sender) {
    auto* env = envelope_pool_->acquire();
    env->flags = 0;

    if constexpr (TypedMessage<T>::is_inlined) {
        memcpy(env->inline_payload, &msg.payload, sizeof(T));
        env->flags |= Envelope::kFlagInlined;
    } else {
        void* buf = mem::allocate(RegionType::kMessage, sizeof(T), sender);
        memcpy(buf, &msg.payload, sizeof(T));
        env->external_payload = buf;
        // flags already kFlagExternal (0)
    }
    env->payload_size = sizeof(T);
    target_mailbox->enqueue(env);
}
```

### Verification:
```bash
ninja -C build test_unit_mailbox test_unit_actor
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*MessageInlining*"
```

---

## Phase 4: Envelope Release — Inline vs External

**Goal:** When releasing an envelope back to the pool, external payloads are
deallocated; inline payloads are not (they're just buffer bytes in the envelope).

### RED — Write failing tests

```cpp
TEST(MessageInlining, InlineEnvelopeReleaseNoDealloc) {
    auto* env = envelope_pool_->acquire();
    env->flags = Envelope::kFlagInlined;
    env->payload_size = 16;

    size_t frees_before = MemoryRegionRegistry::instance()
        .snapshot(RegionType::kMessage).free_count;

    release_envelope(env);  // Should NOT call mem::deallocate()

    size_t frees_after = MemoryRegionRegistry::instance()
        .snapshot(RegionType::kMessage).free_count;
    EXPECT_EQ(frees_after, frees_before);
}

TEST(MessageInlining, ExternalEnvelopeReleaseDeallocs) {
    auto* env = envelope_pool_->acquire();
    void* payload = mem::allocate(RegionType::kMessage, 64, ActorId{1});
    env->external_payload = payload;
    env->flags = 0;  // external
    env->payload_size = 64;

    size_t frees_before = MemoryRegionRegistry::instance()
        .snapshot(RegionType::kMessage).free_count;

    release_envelope(env);

    size_t frees_after = MemoryRegionRegistry::instance()
        .snapshot(RegionType::kMessage).free_count;
    EXPECT_GT(frees_after, frees_before);
}
```

### GREEN — Implement

```cpp
void release_envelope(Envelope* env) {
    if (!(env->flags & Envelope::kFlagInlined) && env->external_payload) {
        mem::deallocate(env->external_payload);
    }
    env->flags = 0;
    env->payload_size = 0;
    envelope_pool_->release(env);
}
```

### Verification:
```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*MessageInlining*"
```

---

## Phase 5: DLQ Preserves Inline Payloads

**Goal:** Dead-lettered messages with inline payloads have their payload
correctly copied to the DLQ record.

### RED — Write failing tests

```cpp
TEST(MessageInlining, DlqPreservesInlinePayload) {
    // Send an inline message that ends up in DLQ (e.g., full mailbox)
    SmallPod data{42, 84};
    // ... send to full mailbox → DLQ ...

    auto records = dead_letter_queue().snapshot_records();
    bool found = false;
    for (auto& r : records) {
        if (r.payload.size() == sizeof(SmallPod)) {
            SmallPod recovered;
            memcpy(&recovered, r.payload.data(), sizeof(SmallPod));
            if (recovered.a == 42 && recovered.b == 84) {
                found = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found) << "Inline payload not found in DLQ";
}

TEST(MessageInlining, DlqExternalPayloadAlsoPreserved) {
    LargePod data;
    memset(&data, 0x42, sizeof(LargePod));
    // ... send to full mailbox → DLQ ...
    auto records = dead_letter_queue().snapshot_records();
    // Same test as above but for external payload
}
```

### GREEN — Implement

The DLQ already uses `envelope->payload_data()` to copy — no change needed.
Just verify the unified accessor works correctly for both paths.

### Verification:
```bash
ninja -C build test_unit_mailbox test_unit_dlq
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*MessageInlining*"
```

---

## Phase 6: Tracing Survives Inline Delivery

**Goal:** `TraceContext` span propagation is unaffected by inlining (trace
context is part of the envelope, not the payload).

### RED — Write failing test

```cpp
TEST(MessageInlining, TraceContextPreservedWithInlinePayload) {
    auto* env = envelope_pool_->acquire();
    // Set trace context on envelope
    TraceContext tc = TraceContext::start_new("inline-test");
    env->trace_id = tc.trace_id();
    env->span_id = tc.span_id();

    // Inline a small payload
    SmallPod data{99, 100};
    memcpy(env->inline_payload, &data, sizeof(SmallPod));
    env->flags |= Envelope::kFlagInlined;
    env->payload_size = sizeof(SmallPod);

    // Verify trace context intact
    EXPECT_EQ(env->trace_id, tc.trace_id());
    EXPECT_EQ(env->span_id, tc.span_id());
    EXPECT_EQ(env->payload_size, sizeof(SmallPod));
}
```

### GREEN — Implement

Tracing is already envelope-level — no change needed. This test validates the
invariant.

### Verification:
```bash
ninja -C build test_unit_mailbox test_unit_tracing
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*MessageInlining*"
```

---

## Phase 7: Integration Stress — 1M Mixed-Size Messages

**Goal:** 1M messages (50% inlined, 50% external) through 4-thread system with
no leaks, no corruption, correct delivery.

### RED — Write failing test

```cpp
TEST(MessageInlining, Stress1MMixedSizeMessages) {
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> inline_count{0};
    std::atomic<uint64_t> external_count{0};

    // Setup: 4 senders, 1 receiver
    // Each sender: 250K messages, 50% inline (<32B), 50% external (>32B)
    // Receiver: counts messages + validates payload integrity

    // ... spawn actors, run test ...

    EXPECT_EQ(received.load(), 1'000'000u);
    EXPECT_GT(inline_count.load(), 400'000u);    // ~50% inline
    EXPECT_GT(external_count.load(), 400'000u);  // ~50% external
    // No leaks: MemoryRegionRegistry shows balanced alloc/free
}
```

### GREEN — Implement

Integration test infrastructure — validates the full stack.

### Verification:
```bash
ninja -C build test_integration_mailbox
./build/tests/integration/mailbox/test_integration_mailbox --gtest_filter="*MessageInlining*"

# Bench saturate quick-saturate: verify ≥20% throughput improvement for 16B payload
./build/apps/bench_saturate --preset quick-saturate
```

---

## Files Changed Summary

| File | Change |
|------|--------|
| `include/hpactor/mailbox/multi_lane_queue.hpp` | Envelope: inline buffer union, `kFlagInlined`, `payload_data()`, `payload_size()` |
| `include/hpactor/types/typed_message.hpp` | `kCanInlinePayload<T>` constexpr, `is_inlined` |
| `src/actor/actor_context.cpp` | Compile-time inline delivery path in `send()` |
| `src/mailbox/multi_lane_queue.cpp` | Envelope release: inline vs external deallocation |
| `tests/unit/mailbox/test_message_inlining.cpp` | **New file** — 12 test cases |
| `tests/unit/mailbox/CMakeLists.txt` | Add new test target |

## Verification Checklist

```bash
# Unit tests
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*MessageInlining*"

# All existing mailbox tests — zero regression
ctest -R "test_unit_mailbox|test_integration_mailbox" --output-on-failure --parallel 8

# Verify zero allocation for inline messages
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*InlineDeliverySkipsAllocation*"

# Bench saturate throughput comparison
./build/apps/bench_saturate --preset quick-saturate  # baseline
# vs pre-MEM-006 baseline: ≥20% throughput improvement for 16B payload

# TSan
cmake -S . -B build-tsan -DENABLE_TSAN=ON && ninja -C build-tsan test_unit_mailbox
```
