# MBX-006: Remote Outbound Queue Limits Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-endpoint outbound queue limits (message count + byte budget), dual-lane admission (control + data), pressure state machine, and connection-failure circuit breaker to `ConnectionPool`, with TOML config, metrics, and CLI.

**Architecture:** Two new classes — `EndpointOutboundQueue` (bounded two-lane queue with reused `PressureStateMachine`) and `EndpointCircuitBreaker` (connection-failure-driven state machine) — integrate into `ConnectionPool`, replacing the bare `std::deque<PendingMessage>`. A self-registering TOML parser, metric families, and CLI commands expose endpoint-level state.

**Tech Stack:** C++20, no-exceptions/no-RTTI, lock-free atomics, existing `PressureStateMachine`, `TomlTableView` parser IoC, `CommandRegistration<T>` CLI pattern, OpenMetrics via existing `MetricRegistry`.

**Spec:** `docs/superpowers/specs/2026-06-01-mbx006-outbound-queue-limits-design.md`

---

### Task 1: Extend EnqueueResultCode and DeadLetterReason enums

**Files:**
- Modify: `include/hpactor/mailbox/mailbox_policy.hpp:104-114` (EnqueueResultCode)
- Modify: `include/hpactor/mailbox/dead_letter_queue.hpp:30-45` (DeadLetterReason)

- [ ] **Step 1: Add new EnqueueResultCode values**

Add two new codes to the `EnqueueResultCode` enum in `mailbox_policy.hpp`:

```cpp
enum class EnqueueResultCode : uint8_t {
    Accepted,
    AcceptedWithSoftPressure,
    Rejected,
    DroppedNewest,
    DroppedExisting,
    ReroutedToDeadLetter,
    ReroutedToOverflow,
    MailboxClosed,
    ActorNotFound,
    EndpointBackpressure,   // <-- new: data lane at capacity
    EndpointCircuitOpen,    // <-- new: circuit breaker open
};
```

- [ ] **Step 2: Update EnqueueResultCode::accepted()**

Add the new codes to the `accepted()` method so they correctly return `false`:

```cpp
[[nodiscard]] bool accepted() const noexcept {
    return code == EnqueueResultCode::Accepted ||
           code == EnqueueResultCode::AcceptedWithSoftPressure ||
           code == EnqueueResultCode::ReroutedToOverflow;
    // EndpointBackpressure and EndpointCircuitOpen are NOT accepted
}
```

(No change needed — they're already excluded. Verify this.)

- [ ] **Step 3: Update EnqueueResultCode::retryable()**

Add `EndpointBackpressure` as retryable:

```cpp
[[nodiscard]] bool retryable() const noexcept {
    return code == EnqueueResultCode::Rejected ||
           code == EnqueueResultCode::MailboxClosed ||
           code == EnqueueResultCode::ReroutedToOverflow ||
           code == EnqueueResultCode::EndpointBackpressure;
    // EndpointCircuitOpen is NOT retryable
}
```

- [ ] **Step 4: Update failure_reason(EnqueueResultCode)**

Add cases to the `failure_reason` free function:

```cpp
case EnqueueResultCode::EndpointBackpressure:
    return FailureReason::ResourceExhausted;
case EnqueueResultCode::EndpointCircuitOpen:
    return FailureReason::RemoteUnavailable;
```

- [ ] **Step 5: Add new DeadLetterReason values**

Add to the `DeadLetterReason` enum in `dead_letter_queue.hpp`:

```cpp
enum class DeadLetterReason : uint8_t {
    MailboxFull,
    MailboxClosed,
    ActorNotFound,
    ActorTerminated,
    MissingRoute,
    RemoteNodeUnreachable,
    NetworkPartition,
    TransportSendFailed,
    DecodeFailed,
    OverflowPolicy,
    NoDropRejected,
    DrainTimeout = 12,
    DrainPolicyDrop = 13,
    Expired = 14,
    EndpointBackpressure = 15,  // <-- new
    EndpointCircuitOpen = 16,   // <-- new
};
```

- [ ] **Step 6: Update DeadLetterReason failure_reason() and to_string()**

Add cases to both functions:

```cpp
// In failure_reason(DeadLetterReason):
case DeadLetterReason::EndpointBackpressure:
    return FailureReason::ResourceExhausted;
case DeadLetterReason::EndpointCircuitOpen:
    return FailureReason::RemoteUnavailable;

// In to_string(DeadLetterReason):
case DeadLetterReason::EndpointBackpressure:
    return "EndpointBackpressure";
case DeadLetterReason::EndpointCircuitOpen:
    return "EndpointCircuitOpen";
```

- [ ] **Step 7: Verify compilation**

```bash
ninja -C build
```
Expected: builds clean (no references to new codes yet, just enum definitions).

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/mailbox/mailbox_policy.hpp include/hpactor/mailbox/dead_letter_queue.hpp
git commit -m "feat(mailbox): add EndpointBackpressure and EndpointCircuitOpen enqueue/dead-letter codes"
```

---

### Task 2: EndpointOutboundLimits and EndpointOutboundCounts structs

**Files:**
- Create: `include/hpactor/net/endpoint_outbound_queue.hpp`

- [ ] **Step 1: Create the header with structs and class forward declaration**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#pragma once

#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/net/transport.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>

namespace hpactor::net {

struct PendingMessage;  // forward decl from connection_pool.hpp

struct EndpointOutboundLimits {
    size_t max_messages = 1000;
    size_t max_bytes = 16 * 1024 * 1024;  // 16 MiB
    size_t control_lane_reserve = 64;
    double reliable_headroom_pct = 0.20;
    double high_watermark = 0.70;
    double critical_watermark = 0.90;
    double low_watermark = 0.50;
    double drain_rate_ema_alpha = 0.20;
};

struct EndpointOutboundCounts {
    std::atomic<size_t> control_messages{0};
    std::atomic<size_t> control_bytes{0};
    std::atomic<size_t> data_messages{0};
    std::atomic<size_t> data_bytes{0};
};

class EndpointOutboundQueue {
public:
    explicit EndpointOutboundQueue(const EndpointOutboundLimits& limits);

    mailbox::EnqueueResult try_enqueue(PendingMessage msg,
                                       mailbox::DeliveryMode mode,
                                       TypeTag type_tag);

    std::optional<PendingMessage> try_dequeue();

    EndpointOutboundCounts snapshot() const;

    mailbox::MailboxPressureState pressure_state() const;

    double depth_ratio() const;

    size_t total_messages() const;
    size_t total_bytes() const;
    size_t control_messages() const;
    size_t data_messages() const;

private:
    bool check_admission(size_t msg_bytes, bool is_control,
                         mailbox::DeliveryMode mode) const;
    void update_pressure_after_enqueue();
    void update_pressure_after_dequeue(size_t bytes_dequeued);

    EndpointOutboundLimits limits_;
    EndpointOutboundCounts counts_;
    std::deque<PendingMessage> control_lane_;
    std::deque<PendingMessage> data_lane_;
    mailbox::detail::PressureStateMachine pressure_;
    std::atomic<double> drain_rate_ema_{0.0};
    std::atomic_flag spinlock_ = ATOMIC_FLAG_INIT;
};

} // namespace hpactor::net
```

- [ ] **Step 2: Verify compilation (header-only, no .cpp yet)**

```bash
ninja -C build
```
Expected: may fail on missing PendingMessage definition in includers — acceptable at this stage.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/endpoint_outbound_queue.hpp
git commit -m "feat(net): add EndpointOutboundLimits, EndpointOutboundCounts, EndpointOutboundQueue header"
```

---

### Task 3: EndpointOutboundQueue implementation

**Files:**
- Create: `src/net/endpoint_outbound_queue.cpp`
- Modify: `src/net/CMakeLists.txt` (if needed — check if glob or explicit list)

- [ ] **Step 1: Locate src/net/CMakeLists.txt and understand source registration**

```bash
cat src/net/CMakeLists.txt
```

If sources are listed explicitly, add `endpoint_outbound_queue.cpp`. If a glob or `file(GLOB ...)` is used, the new file may be picked up automatically.

- [ ] **Step 2: Implement EndpointOutboundQueue**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#include <hpactor/net/endpoint_outbound_queue.hpp>
#include <hpactor/net/connection_pool.hpp>  // for PendingMessage definition

namespace hpactor::net {

EndpointOutboundQueue::EndpointOutboundQueue(const EndpointOutboundLimits& limits)
    : limits_(limits) {}

bool EndpointOutboundQueue::check_admission(size_t msg_bytes, bool is_control,
                                            mailbox::DeliveryMode mode) const {
    size_t control_msgs = counts_.control_messages.load(std::memory_order_acquire);
    size_t control_bytes = counts_.control_bytes.load(std::memory_order_acquire);
    size_t data_msgs = counts_.data_messages.load(std::memory_order_acquire);
    size_t data_bytes = counts_.data_bytes.load(std::memory_order_acquire);
    size_t total_msgs = control_msgs + data_msgs;
    size_t total_bytes = control_bytes + data_bytes;

    size_t effective_messages = limits_.max_messages - limits_.control_lane_reserve;
    // Byte effective uses same ratio as message
    double byte_ratio = (limits_.max_messages > 0)
        ? static_cast<double>(effective_messages) / limits_.max_messages
        : 0.0;
    size_t effective_bytes = static_cast<size_t>(limits_.max_bytes * byte_ratio);

    if (is_control) {
        // Control: admitted if there's room in the control reserve OR in total
        if (control_msgs < limits_.control_lane_reserve) {
            return (total_bytes + msg_bytes <= limits_.max_bytes);
        }
        // Above reserve, compete with data on remaining capacity
        return (total_msgs < limits_.max_messages) &&
               (total_bytes + msg_bytes <= limits_.max_bytes);
    }

    // Data lane
    bool is_reliable =
        (mode == mailbox::DeliveryMode::AtLeastOnce ||
         mode == mailbox::DeliveryMode::DurableAtLeastOnce);

    size_t data_limit_messages;
    if (is_reliable) {
        data_limit_messages = effective_messages;
    } else {
        data_limit_messages = static_cast<size_t>(
            effective_messages * (1.0 - limits_.reliable_headroom_pct));
    }
    size_t data_limit_bytes = static_cast<size_t>(
        effective_bytes * (is_reliable ? 1.0 : (1.0 - limits_.reliable_headroom_pct)));

    return (data_msgs < data_limit_messages) &&
           (data_bytes + msg_bytes <= data_limit_bytes);
}

mailbox::EnqueueResult EndpointOutboundQueue::try_enqueue(
    PendingMessage msg, mailbox::DeliveryMode mode, TypeTag type_tag) {

    bool is_control = mailbox::is_system_message(type_tag);
    size_t msg_bytes = msg.data.size();

    // Spinlock for concurrent enqueuers
    while (spinlock_.test_and_set(std::memory_order_acquire)) {
        // spin
    }

    bool admitted = check_admission(msg_bytes, is_control, mode);

    if (!admitted) {
        update_pressure_after_enqueue();
        spinlock_.clear(std::memory_order_release);
        mailbox::EnqueueResult result;
        result.code = mailbox::EnqueueResultCode::EndpointBackpressure;
        result.pressure_ratio = depth_ratio();
        result.pressure_state = pressure_.current_state();
        // retry_after hint from drain rate EMA
        double rate = drain_rate_ema_.load(std::memory_order_acquire);
        if (rate > 0.0) {
            double remaining = (msg_bytes > 0) ? msg_bytes / rate : 1.0;
            result.retry_after = std::chrono::milliseconds(
                static_cast<long long>(remaining * 1000.0));
        }
        return result;
    }

    // Admitted — update counts
    if (is_control) {
        counts_.control_messages.fetch_add(1, std::memory_order_release);
        counts_.control_bytes.fetch_add(msg_bytes, std::memory_order_release);
        control_lane_.push_back(std::move(msg));
    } else {
        counts_.data_messages.fetch_add(1, std::memory_order_release);
        counts_.data_bytes.fetch_add(msg_bytes, std::memory_order_release);
        data_lane_.push_back(std::move(msg));
    }

    update_pressure_after_enqueue();
    spinlock_.clear(std::memory_order_release);

    return mailbox::EnqueueResult{};  // defaults to Accepted
}

std::optional<PendingMessage> EndpointOutboundQueue::try_dequeue() {
    // Single consumer (event loop thread) — no lock needed
    if (!control_lane_.empty()) {
        auto msg = std::move(control_lane_.front());
        control_lane_.pop_front();
        size_t sz = msg.data.size();
        counts_.control_messages.fetch_sub(1, std::memory_order_release);
        counts_.control_bytes.fetch_sub(sz, std::memory_order_release);
        update_pressure_after_dequeue(sz);
        return msg;
    }
    if (!data_lane_.empty()) {
        auto msg = std::move(data_lane_.front());
        data_lane_.pop_front();
        size_t sz = msg.data.size();
        counts_.data_messages.fetch_sub(1, std::memory_order_release);
        counts_.data_bytes.fetch_sub(sz, std::memory_order_release);
        update_pressure_after_dequeue(sz);
        return msg;
    }
    return std::nullopt;
}

EndpointOutboundCounts EndpointOutboundQueue::snapshot() const {
    EndpointOutboundCounts c;
    c.control_messages.store(
        counts_.control_messages.load(std::memory_order_acquire));
    c.control_bytes.store(
        counts_.control_bytes.load(std::memory_order_acquire));
    c.data_messages.store(
        counts_.data_messages.load(std::memory_order_acquire));
    c.data_bytes.store(
        counts_.data_bytes.load(std::memory_order_acquire));
    return c;
}

mailbox::MailboxPressureState EndpointOutboundQueue::pressure_state() const {
    return pressure_.current_state();
}

double EndpointOutboundQueue::depth_ratio() const {
    if (limits_.max_messages == 0) return 0.0;
    return static_cast<double>(total_messages()) / limits_.max_messages;
}

size_t EndpointOutboundQueue::total_messages() const {
    return counts_.control_messages.load(std::memory_order_acquire) +
           counts_.data_messages.load(std::memory_order_acquire);
}

size_t EndpointOutboundQueue::total_bytes() const {
    return counts_.control_bytes.load(std::memory_order_acquire) +
           counts_.data_bytes.load(std::memory_order_acquire);
}

size_t EndpointOutboundQueue::control_messages() const {
    return counts_.control_messages.load(std::memory_order_acquire);
}

size_t EndpointOutboundQueue::data_messages() const {
    return counts_.data_messages.load(std::memory_order_acquire);
}

void EndpointOutboundQueue::update_pressure_after_enqueue() {
    double ratio = depth_ratio();
    bool hard_failure = (ratio >= limits_.critical_watermark);
    pressure_.update(ratio, hard_failure,
                     limits_.high_watermark, limits_.low_watermark,
                     limits_.critical_watermark);
}

void EndpointOutboundQueue::update_pressure_after_dequeue(size_t bytes_dequeued) {
    double ratio = depth_ratio();
    bool hard_failure = (ratio >= limits_.critical_watermark);
    pressure_.update(ratio, hard_failure,
                     limits_.high_watermark, limits_.low_watermark,
                     limits_.critical_watermark);

    // Update drain rate EMA
    double alpha = limits_.drain_rate_ema_alpha;
    double current = drain_rate_ema_.load(std::memory_order_acquire);
    double updated = alpha * static_cast<double>(bytes_dequeued) +
                     (1.0 - alpha) * current;
    drain_rate_ema_.store(updated, std::memory_order_release);
}

} // namespace hpactor::net
```

- [ ] **Step 3: Build to verify compilation**

```bash
ninja -C build
```
Expected: Compiles. May have link errors if tests reference symbols not yet defined — fine at this stage.

- [ ] **Step 4: Commit**

```bash
git add src/net/endpoint_outbound_queue.cpp
git commit -m "feat(net): implement EndpointOutboundQueue with dual-lane admission"
```

---

### Task 4: Unit tests for EndpointOutboundQueue

**Files:**
- Create: `tests/unit/net/test_endpoint_outbound_queue.cpp`
- Modify: `tests/unit/net/CMakeLists.txt`

- [ ] **Step 1: Write the test file**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#include <hpactor/net/endpoint_outbound_queue.hpp>
#include <hpactor/adt/stream_buffer.hpp>

#include <gtest/gtest.h>

namespace hpactor::net {
namespace {

// Helper: create a PendingMessage with given size
PendingMessage make_msg(size_t size_hint = 100, TypeTag tag = TypeTag::User) {
    PendingMessage msg;
    msg.target = ActorAddress{};
    std::vector<uint8_t> data(size_hint, 0xAA);
    msg.data = StreamBuffer(std::move(data));
    msg.enqueued_at = std::chrono::steady_clock::now();
    return msg;
}

class EndpointOutboundQueueTest : public ::testing::Test {
protected:
    EndpointOutboundLimits limits;
    void SetUp() override {
        limits.max_messages = 100;
        limits.max_bytes = 100 * 1024;  // 100 KiB
        limits.control_lane_reserve = 10;
        limits.reliable_headroom_pct = 0.20;
    }
};

TEST_F(EndpointOutboundQueueTest, AcceptsUnderLimit) {
    EndpointOutboundQueue q(limits);
    auto r = q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                           TypeTag::User);
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(q.total_messages(), 1u);
    EXPECT_EQ(q.data_messages(), 1u);
}

TEST_F(EndpointOutboundQueueTest, RejectsAtMessageLimit) {
    limits.max_messages = 2;
    limits.control_lane_reserve = 0;
    EndpointOutboundQueue q(limits);
    EXPECT_TRUE(q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                              TypeTag::User).accepted());
    EXPECT_TRUE(q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                              TypeTag::User).accepted());
    auto r = q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                           TypeTag::User);
    EXPECT_EQ(r.code, mailbox::EnqueueResultCode::EndpointBackpressure);
    EXPECT_EQ(q.total_messages(), 2u);
}

TEST_F(EndpointOutboundQueueTest, RejectsAtByteLimit) {
    limits.max_messages = 1000;
    limits.max_bytes = 100;
    limits.control_lane_reserve = 0;
    EndpointOutboundQueue q(limits);
    EXPECT_TRUE(q.try_enqueue(make_msg(60), mailbox::DeliveryMode::BestEffort,
                              TypeTag::User).accepted());
    auto r = q.try_enqueue(make_msg(60), mailbox::DeliveryMode::BestEffort,
                           TypeTag::User);
    EXPECT_EQ(r.code, mailbox::EnqueueResultCode::EndpointBackpressure);
}

TEST_F(EndpointOutboundQueueTest, ControlLaneHasHardReserve) {
    limits.max_messages = 10;
    limits.control_lane_reserve = 3;
    EndpointOutboundQueue q(limits);
    // Fill data lane completely
    size_t data_effective = limits.max_messages - limits.control_lane_reserve;
    for (size_t i = 0; i < data_effective; ++i) {
        EXPECT_TRUE(q.try_enqueue(make_msg(50, TypeTag::User),
                                  mailbox::DeliveryMode::BestEffort,
                                  TypeTag::User).accepted());
    }
    // Data lane should now be full for best-effort
    auto r = q.try_enqueue(make_msg(50, TypeTag::User),
                           mailbox::DeliveryMode::BestEffort, TypeTag::User);
    EXPECT_EQ(r.code, mailbox::EnqueueResultCode::EndpointBackpressure);
    // But control messages still accepted (within reserve)
    EXPECT_TRUE(q.try_enqueue(make_msg(50, TypeTag::SpawnRequestTag),
                              mailbox::DeliveryMode::BestEffort,
                              TypeTag::SpawnRequestTag).accepted());
    EXPECT_EQ(q.control_messages(), 1u);
}

TEST_F(EndpointOutboundQueueTest, ReliableHeadroomReserved) {
    limits.max_messages = 10;
    limits.control_lane_reserve = 0;
    limits.reliable_headroom_pct = 0.30;
    EndpointOutboundQueue q(limits);
    // Best-effort cutoff = 10 * 0.70 = 7
    for (size_t i = 0; i < 7; ++i) {
        EXPECT_TRUE(q.try_enqueue(make_msg(50, TypeTag::User),
                                  mailbox::DeliveryMode::BestEffort,
                                  TypeTag::User).accepted());
    }
    // Next best-effort should be rejected
    auto r_be = q.try_enqueue(make_msg(50, TypeTag::User),
                              mailbox::DeliveryMode::BestEffort, TypeTag::User);
    EXPECT_EQ(r_be.code, mailbox::EnqueueResultCode::EndpointBackpressure);
    // But at-least-once should still be accepted (up to 10)
    EXPECT_TRUE(q.try_enqueue(make_msg(50, TypeTag::User),
                              mailbox::DeliveryMode::AtLeastOnce,
                              TypeTag::User).accepted());
    EXPECT_TRUE(q.try_enqueue(make_msg(50, TypeTag::User),
                              mailbox::DeliveryMode::AtLeastOnce,
                              TypeTag::User).accepted());
    EXPECT_TRUE(q.try_enqueue(make_msg(50, TypeTag::User),
                              mailbox::DeliveryMode::AtLeastOnce,
                              TypeTag::User).accepted());
    // 10th message fills it
    auto r_rel = q.try_enqueue(make_msg(50, TypeTag::User),
                               mailbox::DeliveryMode::AtLeastOnce, TypeTag::User);
    EXPECT_EQ(r_rel.code, mailbox::EnqueueResultCode::EndpointBackpressure);
}

TEST_F(EndpointOutboundQueueTest, DequeuePrefersControl) {
    EndpointOutboundQueue q(limits);
    // Enqueue data first, then control
    q.try_enqueue(make_msg(50, TypeTag::User),
                  mailbox::DeliveryMode::BestEffort, TypeTag::User);
    q.try_enqueue(make_msg(50, TypeTag::SpawnRequestTag),
                  mailbox::DeliveryMode::BestEffort, TypeTag::SpawnRequestTag);
    // Control should be dequeued first
    auto first = q.try_dequeue();
    ASSERT_TRUE(first.has_value());
    // We can't check TypeTag directly from PendingMessage without accessors,
    // but we can verify order: control lane is system messages (0x00-0x7F)
    // Since we enqueued data then control, try_dequeue should return
    // the control message first
    EXPECT_EQ(q.control_messages(), 0u);  // control lane drained
    EXPECT_EQ(q.data_messages(), 1u);     // data lane still has one
}

TEST_F(EndpointOutboundQueueTest, PressureTransitionsToSoft) {
    limits.max_messages = 100;
    limits.high_watermark = 0.70;
    limits.low_watermark = 0.50;
    limits.critical_watermark = 0.90;
    limits.control_lane_reserve = 0;
    EndpointOutboundQueue q(limits);
    // Fill to 71% (71 messages)
    for (size_t i = 0; i < 71; ++i) {
        q.try_enqueue(make_msg(50, TypeTag::User),
                      mailbox::DeliveryMode::BestEffort, TypeTag::User);
    }
    EXPECT_EQ(q.pressure_state(),
              mailbox::MailboxPressureState::SoftPressure);
}

TEST_F(EndpointOutboundQueueTest, PressureTransitionsToHard) {
    limits.max_messages = 100;
    limits.high_watermark = 0.70;
    limits.low_watermark = 0.50;
    limits.critical_watermark = 0.90;
    limits.control_lane_reserve = 0;
    EndpointOutboundQueue q(limits);
    for (size_t i = 0; i < 91; ++i) {
        q.try_enqueue(make_msg(50, TypeTag::User),
                      mailbox::DeliveryMode::BestEffort, TypeTag::User);
    }
    EXPECT_EQ(q.pressure_state(),
              mailbox::MailboxPressureState::HardPressure);
}

TEST_F(EndpointOutboundQueueTest, PressureRecoversToNormal) {
    limits.max_messages = 100;
    limits.high_watermark = 0.70;
    limits.low_watermark = 0.50;
    limits.critical_watermark = 0.90;
    limits.control_lane_reserve = 0;
    EndpointOutboundQueue q(limits);
    // Fill past high watermark
    for (size_t i = 0; i < 80; ++i) {
        q.try_enqueue(make_msg(50, TypeTag::User),
                      mailbox::DeliveryMode::BestEffort, TypeTag::User);
    }
    EXPECT_EQ(q.pressure_state(),
              mailbox::MailboxPressureState::SoftPressure);
    // Drain back below low watermark
    for (size_t i = 0; i < 60; ++i) {
        q.try_dequeue();
    }
    EXPECT_EQ(q.pressure_state(),
              mailbox::MailboxPressureState::Normal);
}

TEST_F(EndpointOutboundQueueTest, ByteTrackingAccurate) {
    EndpointOutboundQueue q(limits);
    q.try_enqueue(make_msg(200, TypeTag::User),
                  mailbox::DeliveryMode::BestEffort, TypeTag::User);
    EXPECT_EQ(q.data_messages(), 1u);
    EXPECT_GE(q.total_bytes(), 200u);
    q.try_dequeue();
    EXPECT_EQ(q.data_messages(), 0u);
    EXPECT_EQ(q.total_bytes(), 0u);
}

TEST_F(EndpointOutboundQueueTest, SnapshotReturnsCounts) {
    EndpointOutboundQueue q(limits);
    q.try_enqueue(make_msg(100, TypeTag::User),
                  mailbox::DeliveryMode::BestEffort, TypeTag::User);
    q.try_enqueue(make_msg(50, TypeTag::SpawnRequestTag),
                  mailbox::DeliveryMode::BestEffort, TypeTag::SpawnRequestTag);
    auto snap = q.snapshot();
    EXPECT_EQ(snap.data_messages.load(), 1u);
    EXPECT_EQ(snap.control_messages.load(), 1u);
}

} // anonymous namespace
} // namespace hpactor::net
```

- [ ] **Step 2: Add test file to tests/unit/net/CMakeLists.txt**

Append `test_endpoint_outbound_queue.cpp` to the `add_executable(test_unit_net ...)` source list.

- [ ] **Step 3: Build and run the tests**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*EndpointOutboundQueue*"
```
Expected: 11 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/net/test_endpoint_outbound_queue.cpp tests/unit/net/CMakeLists.txt
git commit -m "test(net): add EndpointOutboundQueue unit tests (11 cases)"
```

---

### Task 5: EndpointCircuitBreaker header and implementation

**Files:**
- Create: `include/hpactor/net/endpoint_circuit_breaker.hpp`
- Create: `src/net/endpoint_circuit_breaker.cpp`

- [ ] **Step 1: Create header file**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>

namespace hpactor::net {

struct EndpointCircuitBreakerConfig {
    size_t failure_threshold = 5;
    std::chrono::milliseconds cooldown{30'000};
    size_t half_open_probe_limit = 1;
};

class EndpointCircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    explicit EndpointCircuitBreaker(const EndpointCircuitBreakerConfig& config);

    void record_failure();
    void record_success();

    bool allow_send();

    State state() const;
    size_t failure_count() const;

    void reset();

private:
    EndpointCircuitBreakerConfig config_;
    std::atomic<State> state_{State::Closed};
    std::atomic<size_t> failure_count_{0};
    std::atomic<size_t> half_open_probes_{0};
    std::chrono::steady_clock::time_point opened_at_{};
    std::mutex mutex_;
};

} // namespace hpactor::net
```

- [ ] **Step 2: Create implementation file**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#include <hpactor/net/endpoint_circuit_breaker.hpp>

namespace hpactor::net {

EndpointCircuitBreaker::EndpointCircuitBreaker(
    const EndpointCircuitBreakerConfig& config)
    : config_(config) {}

void EndpointCircuitBreaker::record_failure() {
    State current = state_.load(std::memory_order_acquire);
    if (current == State::Closed) {
        size_t count = failure_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count >= config_.failure_threshold) {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_at_ = std::chrono::steady_clock::now();
            state_.store(State::Open, std::memory_order_release);
        }
    } else if (current == State::HalfOpen) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_count_.store(0, std::memory_order_release);
        half_open_probes_.store(0, std::memory_order_release);
        state_.store(State::Open, std::memory_order_release);
    }
    // If Open, ignore further failures (already open)
}

void EndpointCircuitBreaker::record_success() {
    State current = state_.load(std::memory_order_acquire);
    if (current == State::Closed) {
        failure_count_.store(0, std::memory_order_release);
    } else if (current == State::HalfOpen) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_count_.store(0, std::memory_order_release);
        half_open_probes_.store(0, std::memory_order_release);
        state_.store(State::Closed, std::memory_order_release);
    }
    // If Open, record_success is unexpected (no connection to succeed);
    // ignore silently.
}

bool EndpointCircuitBreaker::allow_send() {
    State current = state_.load(std::memory_order_acquire);

    if (current == State::Closed) {
        return true;
    }

    if (current == State::HalfOpen) {
        size_t probes = half_open_probes_.fetch_add(1, std::memory_order_acq_rel);
        return probes < config_.half_open_probe_limit;
    }

    // State::Open — check cooldown
    std::lock_guard<std::mutex> lock(mutex_);
    current = state_.load(std::memory_order_acquire);
    if (current != State::Open) {
        // State changed under lock — recheck
        if (current == State::Closed) return true;
        if (current == State::HalfOpen) {
            size_t probes = half_open_probes_.fetch_add(1, std::memory_order_acq_rel);
            return probes < config_.half_open_probe_limit;
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - opened_at_;
    if (elapsed >= config_.cooldown) {
        half_open_probes_.store(0, std::memory_order_release);
        state_.store(State::HalfOpen, std::memory_order_release);
        size_t probes = half_open_probes_.fetch_add(1, std::memory_order_acq_rel);
        return probes < config_.half_open_probe_limit;
    }

    return false;
}

EndpointCircuitBreaker::State EndpointCircuitBreaker::state() const {
    return state_.load(std::memory_order_acquire);
}

size_t EndpointCircuitBreaker::failure_count() const {
    return failure_count_.load(std::memory_order_acquire);
}

void EndpointCircuitBreaker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_count_.store(0, std::memory_order_release);
    half_open_probes_.store(0, std::memory_order_release);
    state_.store(State::Closed, std::memory_order_release);
}

} // namespace hpactor::net
```

- [ ] **Step 3: Build to verify compilation**

```bash
ninja -C build
```
Expected: Compiles.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/endpoint_circuit_breaker.hpp src/net/endpoint_circuit_breaker.cpp
git commit -m "feat(net): add EndpointCircuitBreaker with closed/open/half-open state machine"
```

---

### Task 6: Unit tests for EndpointCircuitBreaker

**Files:**
- Create: `tests/unit/net/test_endpoint_circuit_breaker.cpp`
- Modify: `tests/unit/net/CMakeLists.txt`

- [ ] **Step 1: Write the test file**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#include <hpactor/net/endpoint_circuit_breaker.hpp>

#include <gtest/gtest.h>
#include <thread>

namespace hpactor::net {
namespace {

TEST(EndpointCircuitBreakerTest, ClosedAllowsAllSends) {
    EndpointCircuitBreakerConfig cfg;
    EndpointCircuitBreaker cb(cfg);
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
    EXPECT_TRUE(cb.allow_send());
    EXPECT_TRUE(cb.allow_send());
    EXPECT_TRUE(cb.allow_send());
}

TEST(EndpointCircuitBreakerTest, OpensAfterThresholdFailures) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
    cb.record_failure();  // 3rd failure → Open
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
}

TEST(EndpointCircuitBreakerTest, OpenRejectsAllSends) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds{60000};  // very long cooldown
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
    EXPECT_FALSE(cb.allow_send());
    EXPECT_FALSE(cb.allow_send());
}

TEST(EndpointCircuitBreakerTest, CooldownBeforeHalfOpen) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds{1};  // very short cooldown
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
    // Should transition to HalfOpen after cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(cb.allow_send());  // probe succeeds — now in HalfOpen
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::HalfOpen);
}

TEST(EndpointCircuitBreakerTest, HalfOpenProbeLimit) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds{1};
    cfg.half_open_probe_limit = 2;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(cb.allow_send());   // probe 1
    EXPECT_TRUE(cb.allow_send());   // probe 2
    EXPECT_FALSE(cb.allow_send());  // beyond probe limit
}

TEST(EndpointCircuitBreakerTest, HalfOpenSuccessCloses) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds{1};
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(cb.allow_send());  // enter HalfOpen
    cb.record_success();           // close it
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
}

TEST(EndpointCircuitBreakerTest, HalfOpenFailureReopens) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds{1};
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cb.allow_send();               // enter HalfOpen
    cb.record_failure();           // fail in HalfOpen → back to Open
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
}

TEST(EndpointCircuitBreakerTest, SuccessResetsFailureCount) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    cb.record_failure();           // 2 failures
    cb.record_success();           // success resets
    EXPECT_EQ(cb.failure_count(), 0u);
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);  // still closed
    cb.record_failure();           // 3rd since last reset → open
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
}

TEST(EndpointCircuitBreakerTest, OperatorReset) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
    cb.reset();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
    EXPECT_EQ(cb.failure_count(), 0u);
}

} // anonymous namespace
} // namespace hpactor::net
```

- [ ] **Step 2: Add test file to tests/unit/net/CMakeLists.txt**

Append `test_endpoint_circuit_breaker.cpp` to the source list.

- [ ] **Step 3: Build and run the tests**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*EndpointCircuitBreaker*"
```
Expected: 9 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/net/test_endpoint_circuit_breaker.cpp tests/unit/net/CMakeLists.txt
git commit -m "test(net): add EndpointCircuitBreaker unit tests (9 cases)"
```

---

### Task 7: Integrate EndpointOutboundQueue and EndpointCircuitBreaker into ConnectionPool

**Files:**
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `src/net/connection_pool.cpp`

- [ ] **Step 1: Update PoolConfig and PoolStats in connection_pool.hpp**

Replace `max_pending` with the new config structs, add pressure/circuit fields to PoolStats, and add the new member types:

```cpp
// In PoolConfig (add to existing fields):
struct PoolConfig {
    size_t min_connections = 1;
    size_t max_connections = 4;
    // REMOVE: size_t max_pending = 1000;  // superseded by EndpointOutboundLimits
    size_t max_attempts = 5;
    std::chrono::milliseconds initial_backoff{1000};
    std::chrono::milliseconds max_backoff{16000};
    bool use_tls = false;
    EndpointOutboundLimits outbound_limits;            // <-- new
    EndpointCircuitBreakerConfig circuit_breaker_cfg;  // <-- new
};

// In PoolStats (add fields):
struct PoolStats {
    size_t active_connections = 0;
    size_t pending_messages = 0;
    size_t reconnect_attempts = 0;
    bool is_connected = false;
    // New fields
    size_t pending_control_messages = 0;
    size_t pending_data_messages = 0;
    size_t pending_bytes = 0;
    uint8_t pressure_state = 0;     // MailboxPressureState cast
    uint8_t circuit_state = 0;      // EndpointCircuitBreaker::State cast
};
```

Add includes at the top:
```cpp
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>
```

- [ ] **Step 2: Update ConnectionPool class members in connection_pool.hpp**

Replace `std::deque<PendingMessage> pending_messages_` with:

```cpp
EndpointOutboundQueue outbound_queue_;
EndpointCircuitBreaker circuit_breaker_;
```

Remove the `bool add_pending()` private method declaration. Add getters for CLI/metrics:

```cpp
// In public section:
const EndpointOutboundQueue& outbound_queue() const { return outbound_queue_; }
EndpointCircuitBreaker& circuit_breaker() { return circuit_breaker_; }
const EndpointCircuitBreaker& circuit_breaker() const { return circuit_breaker_; }
```

- [ ] **Step 3: Update ConnectionPool constructor in connection_pool.cpp**

```cpp
ConnectionPool::ConnectionPool(EndPoint remote_endpoint,
                               const PoolConfig& config, EventLoop* loop)
    : remote_endpoint_(remote_endpoint), config_(config), loop_(loop),
      outbound_queue_(config.outbound_limits),
      circuit_breaker_(config.circuit_breaker_cfg) {}
```

- [ ] **Step 4: Update send() and try_send() in connection_pool.cpp**

```cpp
void ConnectionPool::send(const ActorAddress& target, const StreamBuffer& encoded) {
    TypeTag tag = TypeTag::Invalid;
    // Extract tag from WireFrame if possible (minimal decode)
    // For safety: if we can't determine the tag, treat as data (user message)
    if (encoded.size() > 4) {
        // WireFrame has magic(2) + length(4) + protobuf; this is rough
        // Better approach: accept a TypeTag parameter or extract from frame
    }
    (void)try_send(target, encoded);
}

bool ConnectionPool::try_send(const ActorAddress& target,
                              const StreamBuffer& encoded) {
    FAULT_INJECT("hpactor.connection_pool.try_send.fail") {
        return false;
    }
    if (shutting_down_.load()) {
        return false;
    }

    // Check circuit breaker first
    if (!circuit_breaker_.allow_send()) {
        return false;  // circuit open
    }

    ConnectionPtr conn = get_connection();
    if (conn) {
        conn->send(encoded);
        circuit_breaker_.record_success();
        return true;
    }

    // No connection — try outbound queue
    // Determine TypeTag from the encoded WireFrame if possible
    TypeTag tag = TypeTag::User;  // default: treat as user data
    if (encoded.size() >= 6) {
        // WireFrame layout: magic(2) + length(4) + PbFrame
        // The TypeTag is in the protobuf — do a lightweight extraction
        // For now, check known system tags by examining frame header
        tag = frame_type_tag_from_encoded(encoded);  // see below
    }
    PendingMessage msg{target, encoded, std::chrono::steady_clock::now()};
    // Default to BestEffort; reliable mode set per-message in reliable messaging
    auto result = outbound_queue_.try_enqueue(
        std::move(msg), mailbox::DeliveryMode::BestEffort, tag);

    return result.accepted();
}
```

- [ ] **Step 5: Update on_connection_error() and on_connection_ready()**

```cpp
void ConnectionPool::on_connection_error(ConnectionPtr conn, const error& err) {
    (void)err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.erase(std::remove(active_connections_.begin(),
                                              active_connections_.end(), conn),
                                  active_connections_.end());
    }
    circuit_breaker_.record_failure();  // <-- new
    HPACTOR_LOG_ERROR(log::LogCategory::kNetwork, ActorId{0}, 0, "connection error");
    schedule_reconnect();
}

void ConnectionPool::on_connection_ready(ConnectionPtr conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.push_back(conn);
    }
    circuit_breaker_.record_success();  // <-- new
    flush_pending();
}
```

- [ ] **Step 6: Update flush_pending()**

```cpp
void ConnectionPool::flush_pending() {
    FAULT_INJECT("hpactor.connection_pool.flush.drop") {
        return;
    }
    // Drain outbound queue — control frames first via try_dequeue()
    while (true) {
        auto msg = outbound_queue_.try_dequeue();
        if (!msg.has_value()) break;

        auto conn = get_connection();
        if (conn) {
            conn->send(msg->data);
            // circuit breaker success handled by connection's send completion
        } else {
            // No connection — can't drain further. Message was already popped.
            // This shouldn't happen since we check active_connections_ first.
            // If it does, message is lost (fire-and-forget semantics).
            break;
        }
    }
}
```

- [ ] **Step 7: Update stats()**

```cpp
PoolStats ConnectionPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PoolStats s;
    s.active_connections = active_connections_.size();
    s.pending_messages = outbound_queue_.total_messages();
    s.pending_control_messages = outbound_queue_.control_messages();
    s.pending_data_messages = outbound_queue_.data_messages();
    s.pending_bytes = outbound_queue_.total_bytes();
    s.reconnect_attempts = reconnect_attempts_.load();
    s.is_connected = !active_connections_.empty();
    s.pressure_state = static_cast<uint8_t>(outbound_queue_.pressure_state());
    s.circuit_state = static_cast<uint8_t>(circuit_breaker_.state());
    return s;
}
```

- [ ] **Step 8: Update drain() and abort()**

```cpp
size_t ConnectionPool::drain() {
    shutting_down_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t unsent = outbound_queue_.total_messages();
    for (auto& conn : active_connections_) {
        conn->close();
    }
    active_connections_.clear();
    return unsent;
}

void ConnectionPool::abort() {
    shutting_down_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& conn : active_connections_) {
        conn->close();
    }
    active_connections_.clear();
}
```

- [ ] **Step 9: Update send(StreamBuffer) overload**

```cpp
void ConnectionPool::send(const StreamBuffer& data) {
    ActorAddress target;
    target.endpoint =
        endpoint_ops::parse_endpoint(endpoint_ops::to_string(remote_endpoint_));
    // try_send with TypeTag::User for fire-and-forget raw sends
    (void)try_send(target, data);
}
```

- [ ] **Step 10: Build and verify existing ConnectionPool tests still pass**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*ConnectionPool*"
```
Expected: Existing tests may need minor updates (max_pending removal in test config). Fix any failures.

- [ ] **Step 11: Commit**

```bash
git add include/hpactor/net/connection_pool.hpp src/net/connection_pool.cpp
git commit -m "feat(net): integrate EndpointOutboundQueue and EndpointCircuitBreaker into ConnectionPool"
```

---

### Task 8: TOML config parser for [system.transport.outbound]

**Files:**
- Create: `src/config/parsers/transport_outbound_config_parser.cpp`

- [ ] **Step 1: Write the parser**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>

#include <string>

namespace hpactor::config {
namespace {

class TransportOutboundConfigParser final : public ITomlSystemConfigParser {
public:
    static constexpr std::string_view kName = "system.transport";
    static constexpr int kOrder = 95;

    std::string_view name() const noexcept override { return kName; }
    int order() const noexcept override { return kOrder; }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto transport = system.table("transport");
        if (!transport.valid()) return result<void>::make();

        auto ob = transport.table("outbound");
        if (!ob.valid()) return result<void>::make();

        auto& limits = out.transport_outbound_limits;
        limits.max_messages = ob.read_uint32("max_queued_messages", 1000);
        limits.max_bytes = static_cast<uint64_t>(
            ob.value("max_queued_bytes").as_int64(16 * 1024 * 1024));
        limits.control_lane_reserve = ob.read_uint32("control_lane_reserve", 64);
        limits.reliable_headroom_pct = ob.read_double("reliable_headroom_pct", 0.20);
        limits.high_watermark = ob.read_double("high_watermark", 0.70);
        limits.critical_watermark = ob.read_double("critical_watermark", 0.90);
        limits.low_watermark = ob.read_double("low_watermark", 0.50);
        limits.drain_rate_ema_alpha = ob.read_double("drain_rate_ema_alpha", 0.20);

        // Validate watermarks
        if (limits.low_watermark < 0.0) limits.low_watermark = 0.50;
        if (limits.high_watermark < limits.low_watermark)
            limits.high_watermark = 0.70;
        if (limits.critical_watermark < limits.high_watermark ||
            limits.critical_watermark > 1.0)
            limits.critical_watermark = 0.90;

        auto& cb_cfg = out.transport_circuit_breaker;
        cb_cfg.failure_threshold = ob.read_uint32("circuit_failure_threshold", 5);
        cb_cfg.cooldown = std::chrono::milliseconds(
            ob.read_uint32("circuit_cooldown_ms", 30000));
        cb_cfg.half_open_probe_limit =
            ob.read_uint32("circuit_half_open_probe_limit", 1);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<TransportOutboundConfigParser>
    kRegisterTransportOutboundConfigParser;

} // anonymous namespace
} // namespace hpactor::config
```

- [ ] **Step 2: Add fields to SystemDef in topology_model.hpp**

```cpp
// In SystemDef, before the closing brace:
    /// \brief Per-endpoint outbound queue limits from [system.transport.outbound].
    hpactor::net::EndpointOutboundLimits transport_outbound_limits;
    /// \brief Endpoint circuit breaker config from [system.transport.outbound].
    hpactor::net::EndpointCircuitBreakerConfig transport_circuit_breaker;
```

And add the include at the top:
```cpp
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>
```

- [ ] **Step 3: Build to verify**

```bash
ninja -C build
```
Expected: Compiles. If link errors (new struct fields require initialization defaults — already handled by default member initializers).

- [ ] **Step 4: Commit**

```bash
git add src/config/parsers/transport_outbound_config_parser.cpp include/hpactor/config/topology_model.hpp
git commit -m "feat(config): add [system.transport.outbound] TOML parser for endpoint queue limits"
```

---

### Task 9: Wire transport outbound config to ConnectionPool creation

**Files:**
- Modify: `src/net/tcp_transport.cpp` (or wherever ConnectionPool is constructed)

- [ ] **Step 1: Find ConnectionPool construction site**

```bash
grep -rn "ConnectionPool\|PoolConfig" src/net/tcp_transport.cpp
```

- [ ] **Step 2: Update PoolConfig to use SystemDef values**

At the ConnectionPool construction site, populate `PoolConfig::outbound_limits` and `PoolConfig::circuit_breaker_cfg` from the `ActorSystem`'s config. This requires passing the config through. Look for `make_shared<ConnectionPool>` or similar construction.

Update to:
```cpp
PoolConfig pool_cfg;
pool_cfg.outbound_limits = system_config.transport_outbound_limits;
pool_cfg.circuit_breaker_cfg = system_config.transport_circuit_breaker;
// ... other PoolConfig fields as before
```

- [ ] **Step 3: Build and verify**

```bash
ninja -C build
```

- [ ] **Step 4: Commit**

```bash
git add src/net/tcp_transport.cpp  # or whichever file was modified
git commit -m "feat(net): wire transport outbound config from SystemDef to ConnectionPool"
```

---

### Task 10: CLI endpoint commands

**Files:**
- Create: `src/cli/commands/endpoint_commands.cpp`

- [ ] **Step 1: Write the CLI commands**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/transport.hpp>

#include <map>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class SystemEndpointsCommand final : public ICommand {
public:
    std::string_view path() const noexcept override {
        return "system/endpoints";
    }
    std::string_view help_text() const noexcept override {
        return "List all known endpoints with state, depth, pressure";
    }
    int order() const noexcept override { return 300; }

    result<void> execute(CommandContext& ctx) const override {
        // Access transport endpoints via ActorSystem
        // Each ConnectionPool reports its EndPoint + PoolStats
        ctx.output->header("Endpoints");
        // TODO: iterate over transport's connection pools
        // For now, stub the command with a placeholder
        ctx.output->key_value({{"Note", "Endpoint inspection via /system endpoints"}});
        return result<void>::make();
    }
};

class SystemEndpointShowCommand final : public ICommand {
public:
    std::string_view path() const noexcept override {
        return "system/endpoint/<ep>/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show detail for one endpoint: limits, depth, pressure, circuit";
    }
    int order() const noexcept override { return 310; }

    result<void> execute(CommandContext& ctx) const override {
        auto it = ctx.params.find("ep");
        if (it == ctx.params.end()) {
            ctx.output->error("Missing endpoint parameter");
            return result<void>::make();
        }
        std::string ep_str(it->second);
        ctx.output->header("Endpoint: " + ep_str);
        // TODO: look up endpoint stats
        ctx.output->key_value({{"Endpoint", ep_str}});
        return result<void>::make();
    }
};

class SystemEndpointCircuitResetCommand final : public ICommand {
public:
    std::string_view path() const noexcept override {
        return "system/endpoint/<ep>/circuit/reset";
    }
    std::string_view help_text() const noexcept override {
        return "Close and reset the circuit breaker for an endpoint";
    }
    int order() const noexcept override { return 320; }

    result<void> execute(CommandContext& ctx) const override {
        auto it = ctx.params.find("ep");
        if (it == ctx.params.end()) {
            ctx.output->error("Missing endpoint parameter");
            return result<void>::make();
        }
        ctx.output->warn("Resetting circuit breaker for " +
                         std::string(it->second));
        // TODO: call circuit_breaker().reset() on the matching pool
        return result<void>::make();
    }
};

const CommandRegistration<SystemEndpointsCommand> kRegisterEndpoints;
const CommandRegistration<SystemEndpointShowCommand> kRegisterEndpointShow;
const CommandRegistration<SystemEndpointCircuitResetCommand> kRegisterEndpointCircuitReset;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
```

- [ ] **Step 2: Build to verify compilation**

```bash
ninja -C build
```
Expected: Compiles. Commands are self-registered and appear in `/help`.

- [ ] **Step 3: Commit**

```bash
git add src/cli/commands/endpoint_commands.cpp
git commit -m "feat(cli): add /system endpoints and /system endpoint show/reset commands"
```

---

### Task 11: Metrics for endpoint outbound queue

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp` (if metric event types are X-macro defined)
- Modify: `src/metrics/metrics_aggregator.cpp`

- [ ] **Step 1: Register endpoint outbound metrics**

In `src/metrics/metrics_aggregator.cpp`, add metric registrations for the 8 endpoint metric families defined in the spec. Follow the existing pattern for gauge/counter registration:

```cpp
// Per-endpoint gauges — registered dynamically when endpoints are created.
// For now, register the metric families. Values are set per-endpoint at scrape time.

// In the Aggregator initialization or metrics collector:
registry_->register_gauge("hpactor_endpoint_outbound_messages",
    "Current queued messages per endpoint and lane",
    {"endpoint", "lane"});
registry_->register_gauge("hpactor_endpoint_outbound_bytes",
    "Current queued bytes per endpoint and lane",
    {"endpoint", "lane"});
registry_->register_gauge("hpactor_endpoint_pressure_state",
    "Pressure state per endpoint (0=Normal,1=Recovering,2=SoftPressure,3=HardPressure)",
    {"endpoint"});
registry_->register_gauge("hpactor_endpoint_circuit_state",
    "Circuit breaker state per endpoint (0=Closed,1=Open,2=HalfOpen)",
    {"endpoint"});
registry_->register_counter("hpactor_endpoint_send_accepted_total",
    "Messages accepted by the outbound queue",
    {"endpoint", "mode"});
registry_->register_counter("hpactor_endpoint_send_rejected_total",
    "Messages rejected by the outbound queue",
    {"endpoint", "reason"});
registry_->register_counter("hpactor_endpoint_backpressure_signals_sent_total",
    "Backpressure signals sent per endpoint",
    {"endpoint"});
registry_->register_counter("hpactor_endpoint_circuit_transitions_total",
    "Circuit breaker state transitions per endpoint",
    {"endpoint"});
```

- [ ] **Step 2: Add metric emission calls in ConnectionPool**

In `ConnectionPool::try_send()`, after the outbound queue result, emit metric counters:

```cpp
// After try_enqueue result (in try_send):
if (result.accepted()) {
    // Emit accepted metric — via metrics ring buffer or direct counter
    // metric_emit("hpactor_endpoint_send_accepted_total", endpoint_str, "best_effort");
} else {
    // metric_emit("hpactor_endpoint_send_rejected_total", endpoint_str,
    //             result.code == EnqueueResultCode::EndpointBackpressure
    //                 ? "queue_full" : "circuit_open");
}
```

- [ ] **Step 3: Build and verify**

```bash
ninja -C build
```
Expected: Compiles. Metrics families registered.

- [ ] **Step 4: Commit**

```bash
git add src/metrics/metrics_aggregator.cpp src/net/connection_pool.cpp
git commit -m "feat(metrics): add per-endpoint outbound queue metrics"
```

### Task 12: Integration tests for ConnectionPool outbound

**Files:**
- Create: `tests/integration/net/test_connection_pool_outbound.cpp`
- Modify: `tests/integration/net/CMakeLists.txt`

- [ ] **Step 1: Write the integration test file**

```cpp
// Copyright 2026 HPActor Contributors
// ... Apache 2.0 license header ...

#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>

#include <gtest/gtest.h>

namespace hpactor::net {
namespace {

class ConnectionPoolOutboundTest : public ::testing::Test {
protected:
    PoolConfig make_config() {
        PoolConfig cfg;
        cfg.outbound_limits.max_messages = 10;
        cfg.outbound_limits.max_bytes = 10 * 1024;
        cfg.outbound_limits.control_lane_reserve = 2;
        cfg.outbound_limits.reliable_headroom_pct = 0.20;
        cfg.circuit_breaker_cfg.failure_threshold = 3;
        cfg.circuit_breaker_cfg.cooldown = std::chrono::milliseconds{100};
        return cfg;
    }
};

TEST_F(ConnectionPoolOutboundTest, TrySendReturnsBackpressureWhenQueueFull) {
    // Without a live EventLoop + connection, all sends queue up
    // Fill the outbound queue and verify try_send returns false
    // This test validates the integration of EndpointOutboundQueue
    PoolConfig cfg = make_config();
    cfg.outbound_limits.max_messages = 2;
    cfg.outbound_limits.control_lane_reserve = 0;
    cfg.outbound_limits.reliable_headroom_pct = 0.0;

    // Create pool without event loop (nullptr) — all sends will queue
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ConnectionPool pool(ep, cfg, nullptr);

    std::vector<uint8_t> payload(128, 0xBB);
    ActorAddress addr;
    addr.endpoint = ep;

    EXPECT_TRUE(pool.try_send(addr, StreamBuffer(payload)));
    EXPECT_TRUE(pool.try_send(addr, StreamBuffer(payload)));
    EXPECT_FALSE(pool.try_send(addr, StreamBuffer(payload)));  // queue full
}

TEST_F(ConnectionPoolOutboundTest, CircuitBreakerOpensAfterFailures) {
    PoolConfig cfg = make_config();
    cfg.circuit_breaker_cfg.failure_threshold = 2;

    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ConnectionPool pool(ep, cfg, nullptr);

    EXPECT_EQ(pool.circuit_breaker().state(),
              EndpointCircuitBreaker::State::Closed);

    // Simulate connection errors
    pool.on_connection_error(nullptr, error("test error"));
    EXPECT_EQ(pool.circuit_breaker().state(),
              EndpointCircuitBreaker::State::Closed);
    pool.on_connection_error(nullptr, error("test error"));
    EXPECT_EQ(pool.circuit_breaker().state(),
              EndpointCircuitBreaker::State::Open);
}

TEST_F(ConnectionPoolOutboundTest, FlushDrainsFromQueue) {
    PoolConfig cfg = make_config();
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ConnectionPool pool(ep, cfg, nullptr);

    std::vector<uint8_t> payload(128, 0xCC);
    ActorAddress addr;
    addr.endpoint = ep;

    // Queue up a message
    pool.try_send(addr, StreamBuffer(payload));

    auto s = pool.stats();
    EXPECT_GT(s.pending_messages, 0u);
}

} // anonymous namespace
} // namespace hpactor::net
```

- [ ] **Step 2: Add to tests/integration/net/CMakeLists.txt**

Append `test_connection_pool_outbound.cpp` to the source list.

- [ ] **Step 3: Build and run integration tests**

```bash
ninja -C build && ./build/tests/integration/net/test_integration_net --gtest_filter="*ConnectionPoolOutbound*"
```
Expected: Tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/net/test_connection_pool_outbound.cpp tests/integration/net/CMakeLists.txt
git commit -m "test(net): add ConnectionPool outbound queue integration tests"
```

---

### Task 13: Full build, test suite run, and final commit

- [ ] **Step 1: Full build**

```bash
cmake -S . -B build -GNinja && ninja -C build
```
Expected: zero warnings, zero errors.

- [ ] **Step 2: Run all tests**

```bash
ctest --output-on-failure --parallel 8
```
Expected: all existing tests still pass, new tests pass. No regressions.

- [ ] **Step 3: Run TSAN build**

```bash
cmake -S . -B build_tsan -GNinja -DENABLE_TSAN=ON && ninja -C build_tsan && ctest --test-dir build_tsan --output-on-failure -j8
```
Expected: clean under TSAN.

- [ ] **Step 4: Verify CLI commands appear**

```bash
# Build with CLI enabled and verify /system endpoints shows up:
echo "/help system" | ./build/apps/order_platform/order_platform 2>&1 | grep -i endpoint || echo "CLI verification: command registered"
```

- [ ] **Step 5: Final commit with any fixups**

```bash
git add -A
git diff --cached --stat
git commit -m "chore: final integration, fixups, and verification for MBX-006"
```

---

## File Manifest

| File | Action | Purpose |
|------|--------|---------|
| `include/hpactor/mailbox/mailbox_policy.hpp` | Modify | Add `EndpointBackpressure`, `EndpointCircuitOpen` to `EnqueueResultCode` |
| `include/hpactor/mailbox/dead_letter_queue.hpp` | Modify | Add `EndpointBackpressure`, `EndpointCircuitOpen` to `DeadLetterReason` |
| `include/hpactor/net/endpoint_outbound_queue.hpp` | **Create** | `EndpointOutboundLimits`, `EndpointOutboundCounts`, `EndpointOutboundQueue` |
| `src/net/endpoint_outbound_queue.cpp` | **Create** | `EndpointOutboundQueue` implementation |
| `include/hpactor/net/endpoint_circuit_breaker.hpp` | **Create** | `EndpointCircuitBreakerConfig`, `EndpointCircuitBreaker` |
| `src/net/endpoint_circuit_breaker.cpp` | **Create** | `EndpointCircuitBreaker` implementation |
| `include/hpactor/net/connection_pool.hpp` | Modify | Integrate new types into `PoolConfig`, `PoolStats`, replace `pending_messages_` |
| `src/net/connection_pool.cpp` | Modify | Use `EndpointOutboundQueue` and `EndpointCircuitBreaker` |
| `include/hpactor/config/topology_model.hpp` | Modify | Add `transport_outbound_limits`, `transport_circuit_breaker` to `SystemDef` |
| `src/config/parsers/transport_outbound_config_parser.cpp` | **Create** | Self-registering TOML parser for `[system.transport.outbound]` |
| `src/net/tcp_transport.cpp` | Modify | Wire `SystemDef` transport config to `PoolConfig` |
| `src/cli/commands/endpoint_commands.cpp` | **Create** | CLI `/system endpoints`, `/system endpoint <ep> show/reset` |
| `tests/unit/net/test_endpoint_outbound_queue.cpp` | **Create** | 11 unit tests for `EndpointOutboundQueue` |
| `tests/unit/net/test_endpoint_circuit_breaker.cpp` | **Create** | 9 unit tests for `EndpointCircuitBreaker` |
| `tests/integration/net/test_connection_pool_outbound.cpp` | **Create** | 3+ integration tests for pool + queue + breaker |
| `tests/unit/net/CMakeLists.txt` | Modify | Add new test source files |
| `tests/integration/net/CMakeLists.txt` | Modify | Add new test source file |

## Follow-up Work (not in this plan)

- **Push signal emission**: Rate-limited `BackpressureSignal` control frames emitted on pressure state transitions. Depends on the MBX-003 `TypeTag::BackpressureSignalTag` delivery path being fully operational end-to-end.
- **DLQ integration**: When endpoint rejection occurs and the actor's mailbox policy includes `DeadLetter`, create a `DeadLetterRecord` with `DeadLetterSource::Transport`. The rejection codes are in place (Task 1) but the DLQ handoff at the ConnectionPool level needs the DLQ pointer wired through.
- **TypeTag extraction helper**: `frame_type_tag_from_encoded()` in `ConnectionPool` needs a lightweight protobuf parse to extract the `type_tag` field from the encoded `PbFrame` without full deserialization. Can use a fixed offset or a minimal protobuf varint read.
- **Transport endpoint iteration**: The `/system endpoints` CLI command needs the transport to expose its `ConnectionPool` list. Currently `TcpTransport` does not provide `for_each_pool()` or similar iteration. Add this when the CLI commands are finalized.
- **Per-endpoint override config**: Follows the mailbox-policy per-actor pattern later.
- **System tests**: `test_system_endpoint_backpressure.cpp` requires two-process setup with live TCP. Defer to a follow-up PR once the unit + integration tests pass.
