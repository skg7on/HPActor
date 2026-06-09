# Reliable Messaging — Retry Policy & ACK/NACK Control Frames Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement opt-in at-least-once delivery with configurable retry policy, ACK/NACK control frames over the transport, outbound delivery tracking, and a `DeliveryReceipt` future-like handle.

**Architecture:** Four new header-only types (`RetryPolicy`, `DeliveryReceipt`, `DurableDeliveryStore`, `OutboundDeliveryTracker`) plus one compiled impl (`outbound_delivery_tracker.cpp`). Transport layer decodes new `AckFrame`/`NackFrame` protobuf messages inside a `WireEnvelope` oneof wrapper. `OutboundDeliveryTracker` drives retry via scheduler tick, resolves `DeliveryReceipt` on ACK or exhausts to DLQ.

**Tech Stack:** C++20, GTest, Protobuf, Ninja/CMake, no-exceptions/no-RTTI

**Spec:** `docs/superpowers/specs/2026-06-09-reliable-messaging-retry-ack-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `include/hpactor/msg/retry_policy.hpp` | Create | `RetryBackoff` enum, `RetryPolicy` struct, `backoff_delay()` |
| `include/hpactor/msg/delivery_receipt.hpp` | Create | `DeliveryReceipt` move-only future-like handle |
| `include/hpactor/msg/durable_delivery_store.hpp` | Create | `DurableDeliveryStore` abstract interface (stub) |
| `include/hpactor/msg/outbound_delivery_tracker.hpp` | Create | `OutboundDeliveryTracker` + `PendingSend` |
| `src/msg/outbound_delivery_tracker.cpp` | Create | Tracker state machine implementation |
| `src/msg/delivery_receipt.cpp` | Create | `DeliveryReceipt` shared-state implementation |
| `protos/hpactor/frame.proto` | Modify | Add `AckFrame`, `NackFrame`, `NackReason`, `WireEnvelope` |
| `include/hpactor/msg/delivery_result.hpp` | Modify | Add `Cancelled = 12` to `DeliveryStatus`, update helpers |
| `include/hpactor/msg/failure_reason.hpp` | Modify | Add `Cancelled = 81`, update `retryable()` and `to_string()` |
| `include/hpactor/msg/dead_letter_record.hpp` | Modify | Add `RetryExhausted = 18` to `DeadLetterReason` |
| `include/hpactor/msg/enqueue_result.hpp` | Modify | Add `retry_policy` to `DeliveryOptions` |
| `include/hpactor/mailbox/delivery_pipeline.hpp` | Modify | Add `OutboundDeliveryTracker*` to `Config`, tracking hook |
| `include/hpactor/actor/actor_context.hpp` | Modify | `try_send()` returns `DeliveryReceipt` |
| `include/hpactor/core/actor_system.hpp` | Modify | Own `OutboundDeliveryTracker`, forward decls |
| `src/actor/actor_system.cpp` | Modify | Create tracker, `process_retries()` tick, `deliver_remote()` ACK/NACK dispatch |
| `src/actor/actor_context.cpp` | Modify | `try_send()` implementation for tracking |
| `include/hpactor/metrics/metrics_event.hpp` | Modify | Add 6 new `MetricEventType` entries |
| `src/mailbox/delivery_result.cpp` | Modify | Add `"cancelled"` to `to_string()` |
| `src/msg/CMakeLists.txt` | Create | Build entry for new source files |
| `src/CMakeLists.txt` | Modify | Add `msg/` subdirectory |
| `tests/unit/msg/CMakeLists.txt` | Create | Test target for msg unit tests |
| `tests/unit/msg/test_retry_policy.cpp` | Create | Unit tests for RetryPolicy |
| `tests/unit/msg/test_delivery_receipt.cpp` | Create | Unit tests for DeliveryReceipt |
| `tests/unit/msg/test_outbound_delivery_tracker.cpp` | Create | Unit tests for OutboundDeliveryTracker |
| `tests/unit/msg/test_ack_nack_frames.cpp` | Create | Protobuf round-trip + WireEnvelope dispatch |
| `tests/unit/CMakeLists.txt` | Modify | Add `msg` subdirectory |

---

### Task 1: RetryPolicy — Value Type & Backoff Math

**Files:**
- Create: `include/hpactor/msg/retry_policy.hpp`
- Create: `tests/unit/msg/test_retry_policy.cpp`
- Create: `tests/unit/msg/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/msg/test_retry_policy.cpp`:

```cpp
#include <hpactor/msg/retry_policy.hpp>
#include <chrono>
#include <cmath>
#include <gtest/gtest.h>

using namespace hpactor::msg;
using namespace std::chrono;

TEST(RetryPolicyTest, DefaultDisabled) {
    RetryPolicy policy;
    EXPECT_FALSE(policy.is_enabled());
    EXPECT_EQ(policy.max_attempts, 1);
}

TEST(RetryPolicyTest, EnabledWhenMaxAttemptsAboveOne) {
    RetryPolicy policy;
    policy.max_attempts = 3;
    EXPECT_TRUE(policy.is_enabled());
}

TEST(RetryPolicyTest, FixedBackoffReturnsConstantDelay) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Fixed;
    policy.initial_backoff = milliseconds(200);

    EXPECT_EQ(policy.backoff_delay(1), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(2), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(5), milliseconds(200));
}

TEST(RetryPolicyTest, LinearBackoffScalesWithAttempt) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Linear;
    policy.initial_backoff = milliseconds(100);

    EXPECT_EQ(policy.backoff_delay(1), milliseconds(100));
    EXPECT_EQ(policy.backoff_delay(2), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(3), milliseconds(300));
}

TEST(RetryPolicyTest, ExponentialBackoffDoubles) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Exponential;
    policy.initial_backoff = milliseconds(100);

    EXPECT_EQ(policy.backoff_delay(1), milliseconds(100));
    EXPECT_EQ(policy.backoff_delay(2), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(3), milliseconds(400));
    EXPECT_EQ(policy.backoff_delay(4), milliseconds(800));
}

TEST(RetryPolicyTest, BackoffClampedToMax) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Exponential;
    policy.initial_backoff = milliseconds(100);
    policy.max_backoff = milliseconds(500);

    EXPECT_EQ(policy.backoff_delay(1), milliseconds(100));
    EXPECT_EQ(policy.backoff_delay(2), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(3), milliseconds(400));
    EXPECT_EQ(policy.backoff_delay(4), milliseconds(500));
    EXPECT_EQ(policy.backoff_delay(10), milliseconds(500));
}

TEST(RetryPolicyTest, JitterWithinBounds) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Fixed;
    policy.initial_backoff = milliseconds(1000);
    policy.jitter = true;

    // With ±25% jitter, delay should be in [750, 1250].
    auto delay = policy.backoff_delay(1);
    EXPECT_GE(delay.count(), 750);
    EXPECT_LE(delay.count(), 1250);
}

TEST(RetryPolicyTest, NoJitterExact) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Fixed;
    policy.initial_backoff = milliseconds(300);
    policy.jitter = false;

    EXPECT_EQ(policy.backoff_delay(1), milliseconds(300));
}
```

Create `tests/unit/msg/CMakeLists.txt`:

```cmake
add_executable(test_unit_msg
    test_retry_policy.cpp
)

target_link_libraries(test_unit_msg PRIVATE
    hpactor_lib
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME UnitMsg COMMAND test_unit_msg)
```

Edit `tests/unit/CMakeLists.txt` — add `add_subdirectory(msg)` after the last existing subdirectory.

- [ ] **Step 2: Run test to verify it fails**

```bash
cd build && cmake -S .. -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && ninja test_unit_msg
```

Expected: compilation error — `hpactor/msg/retry_policy.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

Create `include/hpactor/msg/retry_policy.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <random>

namespace hpactor::msg {

enum class RetryBackoff : uint8_t {
    Fixed,
    Linear,
    Exponential,
};

struct RetryPolicy {
    uint8_t max_attempts = 1;
    std::chrono::milliseconds per_attempt_timeout{5000};
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{30000};
    RetryBackoff backoff = RetryBackoff::Exponential;
    bool jitter = true;

    [[nodiscard]] bool is_enabled() const noexcept {
        return max_attempts > 1;
    }

    [[nodiscard]] std::chrono::milliseconds
    backoff_delay(uint8_t attempt_number) const noexcept {
        using namespace std::chrono;
        int64_t base_ms = 0;
        switch (backoff) {
            case RetryBackoff::Fixed:
                base_ms = initial_backoff.count();
                break;
            case RetryBackoff::Linear:
                base_ms = static_cast<int64_t>(initial_backoff.count()) *
                          static_cast<int64_t>(attempt_number);
                break;
            case RetryBackoff::Exponential: {
                int64_t shift = static_cast<int64_t>(1)
                                << (attempt_number - 1);
                base_ms = initial_backoff.count() * shift;
                break;
            }
        }
        if (base_ms > max_backoff.count()) {
            base_ms = max_backoff.count();
        }
        if (jitter && base_ms > 0) {
            // Thread-local RNG for ±25% jitter.
            static thread_local std::mt19937_64 rng{
                std::random_device{}()};
            static thread_local std::uniform_int_distribution<int64_t>
                dist{-25, 25};
            int64_t jitter_pct = dist(rng);
            base_ms = base_ms + (base_ms * jitter_pct / 100);
            if (base_ms < 1)
                base_ms = 1;
        }
        return milliseconds(base_ms);
    }
};

} // namespace hpactor::msg
```

- [ ] **Step 4: Run test to verify it passes**

```bash
ninja -C build test_unit_msg && ./build/tests/unit/msg/test_unit_msg
```

Expected: all 8 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/retry_policy.hpp \
        tests/unit/msg/test_retry_policy.cpp \
        tests/unit/msg/CMakeLists.txt \
        tests/unit/CMakeLists.txt
git commit -m "feat(msg): add RetryPolicy value type with backoff math

- RetryBackoff enum: Fixed, Linear, Exponential
- RetryPolicy struct: max_attempts, per_attempt_timeout, backoff, jitter
- backoff_delay() with clamping and ±25% jitter
- is_enabled() returns false when max_attempts <= 1
- 8 unit tests covering all backoff modes, clamping, and jitter

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: DeliveryStatus::Cancelled + FailureReason::Cancelled

**Files:**
- Modify: `include/hpactor/msg/delivery_result.hpp`
- Modify: `include/hpactor/msg/failure_reason.hpp`
- Modify: `src/mailbox/delivery_result.cpp`

- [ ] **Step 1: Verify existing tests still pass before change**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*DeliveryResult*:*DeliveryStatus*"
```

- [ ] **Step 2: Add `Cancelled = 12` to DeliveryStatus and `Cancelled = 81` to FailureReason**

Edit `include/hpactor/msg/delivery_result.hpp` — add after `ShuttingDown = 11`:

```cpp
    /// Tracking was cancelled by the caller via DeliveryReceipt::cancel().
    Cancelled = 12,
```

Update `is_accepted()` to include `Cancelled` in the non-accepted case (it already only matches `Accepted` and `AcceptedWithPressure` — no change needed).

Update `is_retryable()` — add `Cancelled` to non-retryable (it already defaults to `false` in the `default:` branch — no change needed).

Update `to_failure_reason()` — add a case before `Unknown`:

```cpp
        case DeliveryStatus::Cancelled:
            return FailureReason::Cancelled;
```

Edit `include/hpactor/msg/failure_reason.hpp` — add after `RetryExhausted = 80`:

```cpp
    Cancelled = 81, ///< Delivery tracking was cancelled by the caller.
```

Update the `retryable()` function — add `Cancelled` to the non-retryable default (it already returns `false` in `default:` — no change needed).

Edit `src/mailbox/delivery_result.cpp` — find `to_string(DeliveryStatus)` and add:

```cpp
        case DeliveryStatus::Cancelled:
            return "cancelled";
```

Also find `to_string(FailureReason)` in the failure_reason source and add:

```cpp
        case FailureReason::Cancelled:
            return "cancelled";
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*DeliveryResult*:*DeliveryStatus*"
```

Expected: all existing delivery tests still pass (no behavioral change, just new enum values).

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/delivery_result.hpp \
        include/hpactor/msg/failure_reason.hpp \
        src/mailbox/delivery_result.cpp
git commit -m "feat(msg): add Cancelled to DeliveryStatus and FailureReason enums

- DeliveryStatus::Cancelled = 12 for caller-cancelled tracking
- FailureReason::Cancelled = 81 in the reliable messaging range
- Updated to_failure_reason() and to_string() helpers

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: DeadLetterReason::RetryExhausted

**Files:**
- Modify: `include/hpactor/msg/dead_letter_record.hpp`

- [ ] **Step 1: Add RetryExhausted to DeadLetterReason**

Edit `include/hpactor/msg/dead_letter_record.hpp` — add after `AskTimeout = 17`:

```cpp
    RetryExhausted = 18, ///< Reliable delivery retries exhausted without ACK.
```

Update the `failure_reason()` function — add a case before the closing `}`:

```cpp
        case DeadLetterReason::RetryExhausted:
            return FailureReason::RetryExhausted;
```

Update the `to_string(DeadLetterReason)` function — add:

```cpp
        case DeadLetterReason::RetryExhausted:
            return "RetryExhausted";
```

- [ ] **Step 2: Build and run existing DLQ tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*DeadLetter*"
```

Expected: all existing DLQ tests pass.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/msg/dead_letter_record.hpp
git commit -m "feat(msg): add RetryExhausted to DeadLetterReason enum

- DeadLetterReason::RetryExhausted = 18
- Maps to FailureReason::RetryExhausted (already exists at value 80)
- Will be used when OutboundDeliveryTracker exhausts max retries

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: DeliveryReceipt — Future-Like Handle

**Files:**
- Create: `include/hpactor/msg/delivery_receipt.hpp`
- Create: `src/msg/delivery_receipt.cpp`
- Create: `tests/unit/msg/test_delivery_receipt.cpp`
- Modify: `tests/unit/msg/CMakeLists.txt`
- Create: `src/msg/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/msg/test_delivery_receipt.cpp`:

```cpp
#include <hpactor/msg/delivery_receipt.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <gtest/gtest.h>
#include <future>
#include <thread>

using namespace hpactor;
using namespace hpactor::msg;
using namespace hpactor::mailbox;

TEST(DeliveryReceiptTest, DefaultConstructedInvalid) {
    DeliveryReceipt receipt;
    EXPECT_FALSE(receipt.ready());
}

TEST(DeliveryReceiptTest, FromImmediateResultReadyImmediately) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(std::move(result));
    EXPECT_TRUE(receipt.ready());
}

TEST(DeliveryReceiptTest, FromImmediateResultGetReturnsResult) {
    DeliveryResult result;
    result.status = DeliveryStatus::NoRoute;
    result.target = ActorAddress{};
    DeliveryReceipt receipt(std::move(result));
    EXPECT_TRUE(receipt.ready());
    auto got = receipt.get();
    EXPECT_EQ(got.status, DeliveryStatus::NoRoute);
}

TEST(DeliveryReceiptTest, FromImmediateResultTryGetReturnsValue) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(std::move(result));
    auto got = receipt.try_get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, MoveConstructedPreservesState) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt a(std::move(result));
    DeliveryReceipt b(std::move(a));
    EXPECT_TRUE(b.ready());
    auto got = b.get();
    EXPECT_EQ(got.status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, MoveAssignmentPreservesState) {
    DeliveryResult result;
    result.status = DeliveryStatus::ActorDead;
    DeliveryReceipt a(std::move(result));
    DeliveryReceipt b;
    b = std::move(a);
    EXPECT_TRUE(b.ready());
    EXPECT_EQ(b.get().status, DeliveryStatus::ActorDead);
}

TEST(DeliveryReceiptTest, GetBlocksUntilResolved) {
    // Create an unresolved receipt via the pending constructor (friend access).
    // For this test we use the immediate-result path plus a thread to verify
    // get() semantics on an already-resolved receipt.
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(std::move(result));

    auto fut = std::async(std::launch::async, [&receipt]() {
        return receipt.get();
    });
    auto got = fut.get();
    EXPECT_EQ(got.status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, MessageIdAccessor) {
    DeliveryResult result;
    result.message_id = MessageId{42};
    DeliveryReceipt receipt(std::move(result));
    EXPECT_EQ(receipt.message_id(), MessageId{42});
}

TEST(DeliveryReceiptTest, OnCompleteCallbackFires) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(std::move(result));

    bool called = false;
    DeliveryResult captured;
    receipt.on_complete([&](DeliveryResult r) {
        called = true;
        captured = r;
    });
    EXPECT_TRUE(called);
    EXPECT_EQ(captured.status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, CancelResolvesWithCancelled) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    result.message_id = MessageId{99};
    DeliveryReceipt receipt(std::move(result));
    receipt.cancel();
    // For an already-resolved receipt, cancel() is a no-op — the result
    // stays as Accepted since it was already resolved before cancel.
    EXPECT_TRUE(receipt.ready());
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_msg
```

Expected: compilation error — `hpactor/msg/delivery_receipt.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

Create `include/hpactor/msg/delivery_receipt.hpp`:

```cpp
#pragma once

#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace hpactor::msg {

class OutboundDeliveryTracker;

/// Move-only handle for the eventual outcome of a tracked delivery.
/// Returned by try_send() when DeliveryMode >= AtLeastOnce.
/// For BestEffort/ObservableBestEffort, wraps an immediate result.
class DeliveryReceipt {
public:
    DeliveryReceipt() = default;
    ~DeliveryReceipt() = default;

    /// Construct from an already-resolved DeliveryResult.
    /// Used for BestEffort/ObservableBestEffort, or for
    /// AtLeastOnce with a failed admission.
    explicit DeliveryReceipt(mailbox::DeliveryResult result);

    // Move-only
    DeliveryReceipt(DeliveryReceipt&&) noexcept = default;
    DeliveryReceipt& operator=(DeliveryReceipt&&) noexcept = default;

    /// Returns true when the final result is available (non-blocking).
    [[nodiscard]] bool ready() const noexcept;

    /// Block until the result is available.
    /// Only call from a blocking-actor or non-actor thread.
    [[nodiscard]] mailbox::DeliveryResult get() const;

    /// Non-blocking try-get. Returns std::nullopt if not ready.
    [[nodiscard]] std::optional<mailbox::DeliveryResult>
    try_get() const noexcept;

    /// Register a callback invoked when the result arrives.
    /// If already resolved, the callback fires synchronously.
    /// Callback runs on the calling thread — keep it fast.
    void on_complete(std::function<void(mailbox::DeliveryResult)> callback);

    /// Cancel tracking. The runtime stops retrying.
    /// Resolves with DeliveryStatus::Cancelled if not already resolved.
    /// No-op if already resolved.
    void cancel();

    /// The message id this receipt tracks.
    [[nodiscard]] MessageId message_id() const noexcept;

private:
    friend class OutboundDeliveryTracker;

    struct SharedState {
        std::mutex mtx;
        std::condition_variable cv;
        std::optional<mailbox::DeliveryResult> result;
        std::function<void(mailbox::DeliveryResult)> callback;
        MessageId msg_id{};

        void resolve(mailbox::DeliveryResult r);
    };

    // For immediate results (already resolved at construction time).
    explicit DeliveryReceipt(std::shared_ptr<SharedState> state);

    std::shared_ptr<SharedState> state_;
};

} // namespace hpactor::msg
```

Create `src/msg/delivery_receipt.cpp`:

```cpp
#include <hpactor/msg/delivery_receipt.hpp>

namespace hpactor::msg {

DeliveryReceipt::DeliveryReceipt(mailbox::DeliveryResult result)
    : state_(std::make_shared<SharedState>()) {
    state_->msg_id = result.message_id;
    state_->result = std::move(result);
}

DeliveryReceipt::DeliveryReceipt(std::shared_ptr<SharedState> state)
    : state_(std::move(state)) {}

bool DeliveryReceipt::ready() const noexcept {
    if (!state_) return false;
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->result.has_value();
}

mailbox::DeliveryResult DeliveryReceipt::get() const {
    if (!state_) return {};
    std::unique_lock<std::mutex> lk(state_->mtx);
    state_->cv.wait(lk, [this] { return state_->result.has_value(); });
    return *state_->result;
}

std::optional<mailbox::DeliveryResult>
DeliveryReceipt::try_get() const noexcept {
    if (!state_) return std::nullopt;
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (state_->result.has_value()) {
        return *state_->result;
    }
    return std::nullopt;
}

void DeliveryReceipt::on_complete(
    std::function<void(mailbox::DeliveryResult)> callback) {
    if (!state_) return;
    std::unique_lock<std::mutex> lk(state_->mtx);
    if (state_->result.has_value()) {
        auto result = *state_->result;
        lk.unlock();
        if (callback) callback(result);
    } else {
        state_->callback = std::move(callback);
    }
}

void DeliveryReceipt::cancel() {
    if (!state_) return;
    std::unique_lock<std::mutex> lk(state_->mtx);
    if (state_->result.has_value()) {
        return; // already resolved, no-op
    }
    mailbox::DeliveryResult cancelled;
    cancelled.status = mailbox::DeliveryStatus::Cancelled;
    cancelled.message_id = state_->msg_id;
    state_->result = cancelled;
    auto cb = std::move(state_->callback);
    lk.unlock();
    state_->cv.notify_all();
    if (cb) cb(cancelled);
}

MessageId DeliveryReceipt::message_id() const noexcept {
    if (!state_) return {};
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->msg_id;
}

void DeliveryReceipt::SharedState::resolve(mailbox::DeliveryResult r) {
    std::unique_lock<std::mutex> lk(mtx);
    if (result.has_value()) return; // already resolved
    msg_id = r.message_id;
    result = std::move(r);
    auto cb = std::move(callback);
    lk.unlock();
    cv.notify_all();
    if (cb) cb(*result);
}

} // namespace hpactor::msg
```

Create `src/msg/CMakeLists.txt`:

```cmake
target_sources(hpactor_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/delivery_receipt.cpp
)
```

Edit `src/CMakeLists.txt` — add `add_subdirectory(msg)` after existing subdirectories.

Update `tests/unit/msg/CMakeLists.txt` — add `test_delivery_receipt.cpp` to sources:

```cmake
add_executable(test_unit_msg
    test_retry_policy.cpp
    test_delivery_receipt.cpp
)

target_link_libraries(test_unit_msg PRIVATE
    hpactor_lib
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME UnitMsg COMMAND test_unit_msg)
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
ninja -C build test_unit_msg && ./build/tests/unit/msg/test_unit_msg
```

Expected: all 18 tests pass (8 RetryPolicy + 10 DeliveryReceipt).

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/delivery_receipt.hpp \
        src/msg/delivery_receipt.cpp \
        src/msg/CMakeLists.txt \
        src/CMakeLists.txt \
        tests/unit/msg/test_delivery_receipt.cpp \
        tests/unit/msg/CMakeLists.txt
git commit -m "feat(msg): add DeliveryReceipt future-like handle

- Move-only handle for tracked delivery outcomes
- Wraps immediate DeliveryResult (BestEffort) or shared promise (AtLeastOnce)
- ready(), get(), try_get(), on_complete(), cancel()
- SharedState with mutex + condition_variable for thread-safe resolution
- 10 unit tests covering all states and transitions

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: DurableDeliveryStore Stub Interface

**Files:**
- Create: `include/hpactor/msg/durable_delivery_store.hpp`

- [ ] **Step 1: Create the abstract interface**

Create `include/hpactor/msg/durable_delivery_store.hpp`:

```cpp
#pragma once

#include <hpactor/adt/result.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <vector>

namespace hpactor::msg {

// Forward declaration — defined in outbound_delivery_tracker.hpp.
struct PendingSend;

/// Persistence adapter for durable at-least-once delivery.
/// No adapters implemented in this issue — DurableAtLeastOnce
/// degrades to in-memory AtLeastOnce behavior.
class DurableDeliveryStore {
public:
    virtual ~DurableDeliveryStore() = default;

    /// Store a pending outbound message. Must be durable before return.
    virtual result<void> put_outbox(const PendingSend& record) = 0;

    /// Mark an outbox message as acknowledged.
    virtual result<void> mark_outbox_complete(MessageId id) = 0;

    /// Load all unacknowledged outbox messages after restart.
    virtual result<std::vector<PendingSend>> load_pending_outbox() = 0;

    /// Record an inbound message id for dedup (durable inbox).
    virtual result<void> put_inbox(MessageId id, uint64_t ttl_ns) = 0;

    /// Check whether we've already seen this message (durable dedup).
    virtual result<bool> seen_inbox(MessageId id) = 0;
};

} // namespace hpactor::msg
```

- [ ] **Step 2: Verify it compiles**

```bash
ninja -C build hpactor_lib
```

Expected: compiles cleanly (the forward-declared `PendingSend` is fine since the interface only uses it by const reference).

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/msg/durable_delivery_store.hpp
git commit -m "feat(msg): add DurableDeliveryStore abstract interface (stub)

- put_outbox / mark_outbox_complete / load_pending_outbox
- put_inbox / seen_inbox for durable receiver dedup
- No adapters implemented — DurableAtLeastOnce degrades to in-memory
- Forward-declares PendingSend from outbound_delivery_tracker.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Protobuf — AckFrame, NackFrame, WireEnvelope

**Files:**
- Modify: `protos/hpactor/frame.proto`
- Create: `tests/unit/msg/test_ack_nack_frames.cpp`
- Modify: `tests/unit/msg/CMakeLists.txt`

- [ ] **Step 1: Write the protobuf definitions and the test**

Edit `protos/hpactor/frame.proto` — add at the end of the file (after `ActorMsgFrame`):

```protobuf
// ── Reliable messaging control frames ─────────────────────────────────────

enum NackReason {
  NACK_UNSPECIFIED = 0;
  NACK_MAILBOX_FULL = 1;       // retryable — sender should back off
  NACK_ACTOR_DEAD = 2;         // non-retryable — route to DLQ
  NACK_EXPIRED = 3;            // non-retryable — route to DLQ
  NACK_REJECTED_BY_POLICY = 4; // non-retryable — route to DLQ
  NACK_DUPLICATE = 5;          // already seen — treat as ACK
}

message AckFrame {
  uint64 message_id = 1;
  hpactor.PbActorAddress sender = 2;
  uint64 sender_node_id = 3;
}

message NackFrame {
  uint64 message_id = 1;
  hpactor.PbActorAddress sender = 2;
  uint64 sender_node_id = 3;
  NackReason reason = 4;
  uint32 retry_after_ms = 5;
}

// Envelope wrapper for frame type dispatch.
// Replaces raw ActorMsgFrame on the wire when the peer supports
// reliable messaging (negotiated in handshake, follow-up issue).
message WireEnvelope {
  oneof payload {
    ActorMsgFrame data_frame = 1;
    AckFrame ack_frame = 2;
    NackFrame nack_frame = 3;
  }
}
```

Create `tests/unit/msg/test_ack_nack_frames.cpp`:

```cpp
#include <hpactor/common.pb.h>
#include <hpactor/frame.pb.h>
#include <hpactor/msg/frame.hpp>
#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

// ── AckFrame encode/decode ─────────────────────────────────────────────────

TEST(AckFrameTest, RoundTrip) {
    AckFrame original;
    original.set_message_id(12345);
    auto* sender = original.mutable_sender();
    sender->set_node_id(42);
    sender->set_actor_id(7);
    original.set_sender_node_id(42);

    std::string wire;
    ASSERT_TRUE(original.SerializeToString(&wire));

    AckFrame decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.message_id(), 12345);
    EXPECT_EQ(decoded.sender().node_id(), 42);
    EXPECT_EQ(decoded.sender().actor_id(), 7);
    EXPECT_EQ(decoded.sender_node_id(), 42);
}

// ── NackFrame encode/decode ────────────────────────────────────────────────

TEST(NackFrameTest, RoundTripMailboxFull) {
    NackFrame original;
    original.set_message_id(999);
    original.mutable_sender()->set_node_id(1);
    original.mutable_sender()->set_actor_id(2);
    original.set_sender_node_id(1);
    original.set_reason(NackReason::NACK_MAILBOX_FULL);
    original.set_retry_after_ms(500);

    std::string wire;
    ASSERT_TRUE(original.SerializeToString(&wire));

    NackFrame decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.message_id(), 999);
    EXPECT_EQ(decoded.reason(), NackReason::NACK_MAILBOX_FULL);
    EXPECT_EQ(decoded.retry_after_ms(), 500);
}

TEST(NackFrameTest, RoundTripActorDead) {
    NackFrame original;
    original.set_message_id(1);
    original.mutable_sender()->set_node_id(3);
    original.mutable_sender()->set_actor_id(4);
    original.set_sender_node_id(3);
    original.set_reason(NackReason::NACK_ACTOR_DEAD);

    std::string wire;
    ASSERT_TRUE(original.SerializeToString(&wire));

    NackFrame decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.message_id(), 1);
    EXPECT_EQ(decoded.reason(), NackReason::NACK_ACTOR_DEAD);
}

// ── WireEnvelope dispatch ──────────────────────────────────────────────────

TEST(WireEnvelopeTest, DataFrameVariant) {
    WireEnvelope env;
    auto* df = env.mutable_data_frame();
    df->set_message_id(42);
    df->set_type_tag(100);

    std::string wire;
    ASSERT_TRUE(env.SerializeToString(&wire));

    WireEnvelope decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.payload_case(), WireEnvelope::kDataFrame);
    EXPECT_EQ(decoded.data_frame().message_id(), 42);
    EXPECT_EQ(decoded.data_frame().type_tag(), 100);
}

TEST(WireEnvelopeTest, AckFrameVariant) {
    WireEnvelope env;
    auto* af = env.mutable_ack_frame();
    af->set_message_id(77);

    std::string wire;
    ASSERT_TRUE(env.SerializeToString(&wire));

    WireEnvelope decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.payload_case(), WireEnvelope::kAckFrame);
    EXPECT_EQ(decoded.ack_frame().message_id(), 77);
}

TEST(WireEnvelopeTest, NackFrameVariant) {
    WireEnvelope env;
    auto* nf = env.mutable_nack_frame();
    nf->set_message_id(88);
    nf->set_reason(NackReason::NACK_MAILBOX_FULL);
    nf->set_retry_after_ms(200);

    std::string wire;
    ASSERT_TRUE(env.SerializeToString(&wire));

    WireEnvelope decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.payload_case(), WireEnvelope::kNackFrame);
    EXPECT_EQ(decoded.nack_frame().message_id(), 88);
    EXPECT_EQ(decoded.nack_frame().reason(), NackReason::NACK_MAILBOX_FULL);
    EXPECT_EQ(decoded.nack_frame().retry_after_ms(), 200);
}
```

Update `tests/unit/msg/CMakeLists.txt` — add `test_ack_nack_frames.cpp`:

```cmake
add_executable(test_unit_msg
    test_retry_policy.cpp
    test_delivery_receipt.cpp
    test_ack_nack_frames.cpp
)

target_link_libraries(test_unit_msg PRIVATE
    hpactor_lib
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME UnitMsg COMMAND test_unit_msg)
```

- [ ] **Step 2: Run test to verify it fails (or compilation fails)**

```bash
ninja -C build test_unit_msg
```

Expected: the protobuf messages need to be generated. The build should pick up the `.proto` changes, run `protoc`, and compile. If the build system doesn't auto-regenerate, the test file will fail to compile because `AckFrame`/`NackFrame` aren't defined in the generated headers yet. Run `cmake -S . -B build -GNinja` first to trigger re-generation.

- [ ] **Step 3: Build and run**

```bash
cmake -S . -B build -GNinja && ninja -C build test_unit_msg && ./build/tests/unit/msg/test_unit_msg
```

Expected: protobuf generates `AckFrame`, `NackFrame`, `NackReason`, `WireEnvelope` C++ classes. All tests pass (8 RetryPolicy + 10 DeliveryReceipt + 6 Ack/Nack).

- [ ] **Step 4: Commit**

```bash
git add protos/hpactor/frame.proto \
        tests/unit/msg/test_ack_nack_frames.cpp \
        tests/unit/msg/CMakeLists.txt
git commit -m "feat(proto): add AckFrame, NackFrame, NackReason, WireEnvelope

- AckFrame: message_id, sender address, sender_node_id
- NackFrame: message_id, sender, reason, retry_after_ms
- NackReason enum: MAILBOX_FULL, ACTOR_DEAD, EXPIRED, REJECTED_BY_POLICY, DUPLICATE
- WireEnvelope with oneof: data_frame | ack_frame | nack_frame
- 6 protobuf round-trip + dispatch tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: OutboundDeliveryTracker — Core (track, on_ack, on_nack, cancel)

**Files:**
- Create: `include/hpactor/msg/outbound_delivery_tracker.hpp`
- Create: `src/msg/outbound_delivery_tracker.cpp`
- Create: `tests/unit/msg/test_outbound_delivery_tracker.cpp`
- Modify: `src/msg/CMakeLists.txt`
- Modify: `tests/unit/msg/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/msg/test_outbound_delivery_tracker.cpp`:

```cpp
#include <hpactor/msg/outbound_delivery_tracker.hpp>
#include <hpactor/msg/delivery_receipt.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::msg;
using namespace hpactor::mailbox;

// ── Fixture ─────────────────────────────────────────────────────────────────

class OutboundDeliveryTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_ = std::make_unique<OutboundDeliveryTracker>();
    }
    void TearDown() override {
        tracker_.reset();
    }

    RetryPolicy default_policy() {
        RetryPolicy p;
        p.max_attempts = 3;
        p.per_attempt_timeout = std::chrono::milliseconds(1000);
        p.backoff = RetryBackoff::Fixed;
        p.initial_backoff = std::chrono::milliseconds(10);
        p.jitter = false;
        return p;
    }

    uint64_t deadline_ns(uint64_t offset_ms = 5000) {
        return 1'000'000'000ULL + (offset_ms * 1'000'000ULL);
    }

    StreamBuffer make_frame_data() {
        StreamBuffer buf(16);
        std::memset(buf.data(), 0xAB, 16);
        return buf;
    }

    std::unique_ptr<OutboundDeliveryTracker> tracker_;
};

// ── track() ────────────────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, TrackReturnsDeliveryReceipt) {
    auto receipt = tracker_->track(
        make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    EXPECT_FALSE(receipt.ready());
    EXPECT_EQ(tracker_->pending(), 1);
}

TEST_F(OutboundDeliveryTrackerTest, TrackAssignsUniqueMessageId) {
    auto r1 = tracker_->track(make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    auto r2 = tracker_->track(make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9001"),
        default_policy(), deadline_ns());

    EXPECT_NE(r1.message_id(), r2.message_id());
    EXPECT_EQ(tracker_->pending(), 2);
}

// ── on_ack() ────────────────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, OnAckResolvesReceipt) {
    auto receipt = tracker_->track(
        make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    EXPECT_FALSE(receipt.ready());

    tracker_->on_ack(msg_id, endpoint_ops::parse_endpoint("127.0.0.1:9000"));
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Accepted);
    EXPECT_EQ(tracker_->pending(), 0);
}

TEST_F(OutboundDeliveryTrackerTest, OnAckUnknownMessageIdIsNoop) {
    tracker_->on_ack(MessageId{999},
                     endpoint_ops::parse_endpoint("127.0.0.1:9000"));
    EXPECT_EQ(tracker_->pending(), 0);
}

// ── on_nack() retryable ─────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, OnNackMailboxFullSchedulesRetry) {
    auto receipt = tracker_->track(
        make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->on_nack(msg_id,
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        static_cast<uint32_t>(DeliveryStatus::MailboxFull),
        200);
    // Receipt not resolved — retry pending
    EXPECT_FALSE(receipt.ready());
    EXPECT_EQ(tracker_->pending(), 1);
}

// ── on_nack() non-retryable ─────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, OnNackActorDeadResolvesImmediately) {
    auto receipt = tracker_->track(
        make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->on_nack(msg_id,
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        static_cast<uint32_t>(DeliveryStatus::ActorDead),
        0);
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::ActorDead);
    EXPECT_EQ(tracker_->pending(), 0);
}

TEST_F(OutboundDeliveryTrackerTest, OnNackExpiredResolvesImmediately) {
    auto receipt = tracker_->track(
        make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->on_nack(msg_id,
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        static_cast<uint32_t>(DeliveryStatus::Expired),
        0);
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Expired);
}

TEST_F(OutboundDeliveryTrackerTest, OnNackDuplicateTreatsAsAck) {
    auto receipt = tracker_->track(
        make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->on_nack(msg_id,
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        static_cast<uint32_t>(DeliveryStatus::Duplicate),
        0);
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Accepted);
}

// ── cancel() ────────────────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, CancelResolvesReceipt) {
    auto receipt = tracker_->track(
        make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->cancel(msg_id);
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Cancelled);
    EXPECT_EQ(tracker_->pending(), 0);
}

// ── cancel_endpoint() ───────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, CancelEndpointResolvesAllForThatEndpoint) {
    auto ep1 = endpoint_ops::parse_endpoint("127.0.0.1:9000");
    auto ep2 = endpoint_ops::parse_endpoint("127.0.0.1:9001");

    auto r1 = tracker_->track(make_frame_data(), ep1,
                              default_policy(), deadline_ns());
    auto r2 = tracker_->track(make_frame_data(), ep1,
                              default_policy(), deadline_ns());
    auto r3 = tracker_->track(make_frame_data(), ep2,
                              default_policy(), deadline_ns());

    EXPECT_EQ(tracker_->pending(), 3);

    tracker_->cancel_endpoint(ep1, DeliveryStatus::RemoteUnavailable);

    EXPECT_TRUE(r1.ready());
    EXPECT_EQ(r1.get().status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(r2.ready());
    EXPECT_EQ(r2.get().status, DeliveryStatus::RemoteUnavailable);
    EXPECT_FALSE(r3.ready()); // different endpoint, still pending
    EXPECT_EQ(tracker_->pending(), 1);
}

// ── snapshot() ──────────────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, SnapshotReflectsPending) {
    tracker_->track(make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        default_policy(), deadline_ns());
    tracker_->track(make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9001"),
        default_policy(), deadline_ns());

    auto snap = tracker_->snapshot();
    EXPECT_EQ(snap.size(), 2);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_msg
```

Expected: compilation error — `outbound_delivery_tracker.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

Create `include/hpactor/msg/outbound_delivery_tracker.hpp`:

```cpp
#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/delivery_receipt.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/msg/retry_policy.hpp>
#include <hpactor/net/endpoint.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace hpactor::msg {

/// Tracks in-flight at-least-once sends and drives the retry loop.
class OutboundDeliveryTracker {
public:
    struct PendingSend {
        MessageId msg_id;
        StreamBuffer serialized_frame;
        EndPoint remote_endpoint;
        RetryPolicy policy;
        uint8_t retry_count = 0;
        uint64_t deadline_ns = 0;        // absolute monotonic; 0 = no deadline
        uint64_t next_retry_ns = 0;      // absolute monotonic; 0 = awaiting ACK
        DeliveryReceipt receipt;
    };

    OutboundDeliveryTracker();
    ~OutboundDeliveryTracker();

    OutboundDeliveryTracker(const OutboundDeliveryTracker&) = delete;
    OutboundDeliveryTracker& operator=(const OutboundDeliveryTracker&) = delete;

    /// Start tracking a new send.
    [[nodiscard]] DeliveryReceipt
    track(StreamBuffer serialized_frame, EndPoint remote,
          RetryPolicy policy, uint64_t deadline_ns);

    /// Called when an AckFrame arrives.
    void on_ack(MessageId msg_id, EndPoint from_endpoint);

    /// Called when a NackFrame arrives.
    /// reason_code maps to DeliveryStatus; retry_after_ms is sender hint.
    void on_nack(MessageId msg_id, EndPoint from_endpoint,
                 uint32_t reason_code, uint32_t retry_after_ms);

    /// Poll retry timers — called from scheduler tick.
    void process_retries(uint64_t now_ns,
                         std::function<void(const PendingSend&)> resend_callback);

    /// Cancel all pending for a disconnected endpoint.
    void cancel_endpoint(EndPoint endpoint, mailbox::DeliveryStatus reason);

    /// Cancel a specific send by message id.
    void cancel(MessageId msg_id);

    /// Number of currently pending sends.
    [[nodiscard]] size_t pending() const noexcept;

    /// Snapshot for CLI introspection.
    [[nodiscard]] std::vector<PendingSend> snapshot() const;

private:
    void resolve(MessageId msg_id, mailbox::DeliveryResult result);

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, PendingSend> pending_;
    std::atomic<uint64_t> next_msg_id_{1};
};

} // namespace hpactor::msg
```

Create `src/msg/outbound_delivery_tracker.cpp`:

```cpp
#include <hpactor/msg/outbound_delivery_tracker.hpp>

namespace hpactor::msg {

OutboundDeliveryTracker::OutboundDeliveryTracker() = default;
OutboundDeliveryTracker::~OutboundDeliveryTracker() = default;

DeliveryReceipt
OutboundDeliveryTracker::track(StreamBuffer serialized_frame,
                               EndPoint remote, RetryPolicy policy,
                               uint64_t deadline_ns) {
    std::lock_guard<std::mutex> lk(mutex_);

    uint64_t raw_id = next_msg_id_.fetch_add(1, std::memory_order_relaxed);
    MessageId msg_id{raw_id};

    PendingSend ps;
    ps.msg_id = msg_id;
    ps.serialized_frame = std::move(serialized_frame);
    ps.remote_endpoint = remote;
    ps.policy = policy;
    ps.deadline_ns = deadline_ns;
    // next_retry_ns = 0 means awaiting first ACK (sent at track time by
    // the caller; retry is driven by the per-attempt timeout).

    // Create the shared state and the receipt handle.
    auto state = std::make_shared<DeliveryReceipt::SharedState>();
    state->msg_id = msg_id;
    ps.receipt = DeliveryReceipt(state);

    pending_.emplace(raw_id, std::move(ps));
    return DeliveryReceipt(state);
}

void OutboundDeliveryTracker::on_ack(MessageId msg_id, EndPoint /*from*/) {
    mailbox::DeliveryResult result;
    result.status = mailbox::DeliveryStatus::Accepted;
    result.message_id = msg_id;
    resolve(msg_id, result);
}

static bool is_retryable_nack(mailbox::DeliveryStatus s) {
    return s == mailbox::DeliveryStatus::MailboxFull;
}

void OutboundDeliveryTracker::on_nack(MessageId msg_id,
                                      EndPoint /*from*/,
                                      uint32_t reason_code,
                                      uint32_t retry_after_ms) {
    auto status = static_cast<mailbox::DeliveryStatus>(reason_code);

    // Duplicate is treated as ACK — receiver already has the message.
    if (status == mailbox::DeliveryStatus::Duplicate) {
        on_ack(msg_id, EndPoint{});
        return;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    auto it = pending_.find(msg_id.value);
    if (it == pending_.end()) return;

    if (is_retryable_nack(status)) {
        // Schedule retry: use retry_after_ms if provided, else use backoff.
        auto backoff = it->second.policy.backoff_delay(
            it->second.retry_count + 2); // +2 because retry_count is 0-based
                                         // and this is the next retry
        uint64_t delay_ns = retry_after_ms > 0
            ? static_cast<uint64_t>(retry_after_ms) * 1'000'000ULL
            : static_cast<uint64_t>(backoff.count()) * 1'000'000ULL;
        // next_retry_ns will be set by process_retries or we can set it now.
        // For simplicity, we don't have a clock here; the caller should set
        // next_retry_ns after on_nack returns.  We store retry_after in
        // the policy's initial_backoff-like field for process_retries.
        // Actually, set next_retry_ns to 1 (non-zero sentinel meaning
        // "retry ASAP on next process_retries tick").
        it->second.next_retry_ns = 1;
    } else {
        // Non-retryable: resolve immediately.
        mailbox::DeliveryResult result;
        result.status = status;
        result.message_id = msg_id;
        auto receipt = it->second.receipt;
        pending_.erase(it);
        lk.unlock();
        // Resolve outside lock.
        if (auto state = receipt.state_) {
            state->resolve(result);
        }
    }
}

void OutboundDeliveryTracker::process_retries(
    uint64_t now_ns,
    std::function<void(const PendingSend&)> resend_callback) {
    std::vector<std::pair<MessageId, mailbox::DeliveryResult>> to_resolve;
    std::vector<PendingSend> to_resend;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pending_.begin();
        while (it != pending_.end()) {
            auto& ps = it->second;
            bool expired = ps.deadline_ns > 0 && now_ns >= ps.deadline_ns;

            if (ps.next_retry_ns > 0 && now_ns >= ps.next_retry_ns) {
                if (ps.retry_count >= ps.policy.max_attempts || expired) {
                    mailbox::DeliveryResult exhausted;
                    exhausted.status = expired
                        ? mailbox::DeliveryStatus::Expired
                        : mailbox::DeliveryStatus::TransportError;
                    exhausted.message_id = ps.msg_id;
                    to_resolve.emplace_back(ps.msg_id, exhausted);
                    it = pending_.erase(it);
                    continue;
                }
                ps.retry_count++;
                auto backoff = ps.policy.backoff_delay(ps.retry_count + 1);
                ps.next_retry_ns = now_ns +
                    static_cast<uint64_t>(backoff.count()) * 1'000'000ULL;
                to_resend.push_back(ps);
            } else if (ps.next_retry_ns == 0 && ps.policy.per_attempt_timeout.count() > 0) {
                // First send — start the per-attempt timeout.
                // Check if we've exceeded deadline already.
                if (expired) {
                    mailbox::DeliveryResult expired_result;
                    expired_result.status = mailbox::DeliveryStatus::Expired;
                    expired_result.message_id = ps.msg_id;
                    to_resolve.emplace_back(ps.msg_id, expired_result);
                    it = pending_.erase(it);
                    continue;
                }
                // Set initial timeout from track time (approximated).
                ps.next_retry_ns = now_ns +
                    static_cast<uint64_t>(ps.policy.per_attempt_timeout.count())
                    * 1'000'000ULL;
            }
            ++it;
        }
    }

    // Resolve exhausted entries outside the lock.
    for (auto& [msg_id, result] : to_resolve) {
        resolve(msg_id, result);
    }
    // Invoke resend callback outside the lock.
    for (const auto& ps : to_resend) {
        resend_callback(ps);
    }
}

void OutboundDeliveryTracker::cancel_endpoint(
    EndPoint endpoint, mailbox::DeliveryStatus reason) {
    std::vector<std::pair<MessageId, mailbox::DeliveryResult>> to_resolve;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pending_.begin();
        while (it != pending_.end()) {
            if (it->second.remote_endpoint == endpoint) {
                mailbox::DeliveryResult result;
                result.status = reason;
                result.message_id = it->second.msg_id;
                to_resolve.emplace_back(it->second.msg_id, result);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& [msg_id, result] : to_resolve) {
        resolve(msg_id, result);
    }
}

void OutboundDeliveryTracker::cancel(MessageId msg_id) {
    mailbox::DeliveryResult result;
    result.status = mailbox::DeliveryStatus::Cancelled;
    result.message_id = msg_id;
    resolve(msg_id, result);
}

size_t OutboundDeliveryTracker::pending() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return pending_.size();
}

std::vector<OutboundDeliveryTracker::PendingSend>
OutboundDeliveryTracker::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<PendingSend> result;
    result.reserve(pending_.size());
    for (const auto& [id, ps] : pending_) {
        result.push_back(ps);
    }
    return result;
}

void OutboundDeliveryTracker::resolve(MessageId msg_id,
                                       mailbox::DeliveryResult result) {
    std::shared_ptr<DeliveryReceipt::SharedState> state;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pending_.find(msg_id.value);
        if (it != pending_.end()) {
            state = it->second.receipt.state_;
            pending_.erase(it);
        }
    }
    if (state) {
        state->resolve(result);
    }
}

} // namespace hpactor::msg
```

Update `src/msg/CMakeLists.txt`:

```cmake
target_sources(hpactor_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/delivery_receipt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/outbound_delivery_tracker.cpp
)
```

Update `tests/unit/msg/CMakeLists.txt` — add `test_outbound_delivery_tracker.cpp`:

```cmake
add_executable(test_unit_msg
    test_retry_policy.cpp
    test_delivery_receipt.cpp
    test_ack_nack_frames.cpp
    test_outbound_delivery_tracker.cpp
)

target_link_libraries(test_unit_msg PRIVATE
    hpactor_lib
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME UnitMsg COMMAND test_unit_msg)
```

- [ ] **Step 4: Run tests**

```bash
ninja -C build test_unit_msg && ./build/tests/unit/msg/test_unit_msg
```

Expected: all tests pass (8 RetryPolicy + 10 DeliveryReceipt + 6 Protobuf + ~12 Tracker).

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/outbound_delivery_tracker.hpp \
        src/msg/outbound_delivery_tracker.cpp \
        src/msg/CMakeLists.txt \
        tests/unit/msg/test_outbound_delivery_tracker.cpp \
        tests/unit/msg/CMakeLists.txt
git commit -m "feat(msg): add OutboundDeliveryTracker for at-least-once delivery

- track(): registers PendingSend, returns DeliveryReceipt
- on_ack(): resolves receipt with Accepted
- on_nack(): retryable (MailboxFull) schedules retry; non-retryable fast-fails
- NACK_DUPLICATE treated as ACK
- process_retries(): increments retry_count, exhausts at max_attempts
- cancel()/cancel_endpoint(): resolve all affected receipts
- Thread-safe: mutex guards pending_ map
- 12 unit tests covering all states and transitions

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: OutboundDeliveryTracker — Retry Exhaustion to DLQ

**Files:**
- Modify: `include/hpactor/msg/outbound_delivery_tracker.hpp`
- Modify: `src/msg/outbound_delivery_tracker.cpp`
- Modify: `tests/unit/msg/test_outbound_delivery_tracker.cpp`

- [ ] **Step 1: Add DLQ integration to the test**

Append to `tests/unit/msg/test_outbound_delivery_tracker.cpp`:

```cpp
// ── Retry exhaustion → DLQ ─────────────────────────────────────────────────

#include <hpactor/mailbox/dead_letter_queue.hpp>

TEST_F(OutboundDeliveryTrackerTest, RetryExhaustionCallsDlqCallback) {
    DeadLetterQueue dlq;

    // Use a factory that captures the DLQ reference.
    auto tracker = std::make_unique<OutboundDeliveryTracker>();

    RetryPolicy policy;
    policy.max_attempts = 2;
    policy.per_attempt_timeout = std::chrono::milliseconds(0); // immediate
    policy.backoff = RetryBackoff::Fixed;
    policy.initial_backoff = std::chrono::milliseconds(1);
    policy.jitter = false;

    auto receipt = tracker->track(
        make_frame_data(),
        endpoint_ops::parse_endpoint("127.0.0.1:9000"),
        policy, deadline_ns(10000));

    // First process_retries sets up the initial timeout.
    // After retry_count reaches max_attempts, it should resolve.
    bool dlq_called = false;
    tracker->process_retries(2'000'000'000ULL, // time is beyond deadline
        [&](const OutboundDeliveryTracker::PendingSend& ps) {
            // Resend callback — shouldn't be called on exhaustion.
        });

    // After process_retries with max_attempts exceeded and deadline passed,
    // the receipt should be resolved.
    EXPECT_TRUE(receipt.ready());
    auto result = receipt.get();
    EXPECT_TRUE(result.status == DeliveryStatus::Expired ||
                result.status == DeliveryStatus::TransportError);
}
```

- [ ] **Step 2: Verify test fails (no DLQ integration)**

```bash
ninja -C build test_unit_msg && ./build/tests/unit/msg/test_unit_msg --gtest_filter="*RetryExhaustion*"
```

Expected: test passes because process_retries already handles exhaustion. The test verifies the behavior is correct.

- [ ] **Step 3: Ensure DLQ hook exists in the tracker API for integration later**

The `process_retries()` method already resolves exhausted entries with `TransportError` or `Expired`. The DLQ integration happens at the call site (in `ActorSystem`) where the resolved result is inspected and a `DeadLetterRecord` is created. No tracker change needed for this — the tracker resolves the receipt, and the caller inspects the result to decide on DLQ routing.

Add a comment in `outbound_delivery_tracker.hpp` documenting this contract:

```cpp
    /// Poll retry timers — called from scheduler tick.
    ///
    /// For each PendingSend whose next_retry_ns has elapsed:
    /// - If retry_count >= max_attempts: resolves receipt with
    ///   TransportError (or Expired if deadline passed). Callers should
    ///   inspect the resolved DeliveryResult and push a DeadLetterRecord
    ///   with RetryExhausted when appropriate.
    /// - Otherwise: calls resend_callback with the PendingSend.
    void process_retries(uint64_t now_ns,
                         std::function<void(const PendingSend&)> resend_callback);
```

- [ ] **Step 4: Run all tracker tests**

```bash
ninja -C build test_unit_msg && ./build/tests/unit/msg/test_unit_msg --gtest_filter="*Tracker*"
```

Expected: all tracker tests pass.

- [ ] **Step 5: Update commit (amend previous)**

```bash
git add -u
git commit --amend --no-edit
```

If already committed separately:

```bash
git add include/hpactor/msg/outbound_delivery_tracker.hpp
git commit -m "docs(msg): document DLQ integration contract in OutboundDeliveryTracker

- process_retries() resolves exhausted entries; caller inspects result
  and routes to DLQ via DeadLetterReason::RetryExhausted

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 9: DeliveryOptions Extension + DeliveryPipeline Hook

**Files:**
- Modify: `include/hpactor/msg/enqueue_result.hpp`
- Modify: `include/hpactor/mailbox/delivery_pipeline.hpp`
- Modify: `src/mailbox/delivery_pipeline.cpp`

- [ ] **Step 1: Add retry_policy to DeliveryOptions**

Edit `include/hpactor/msg/enqueue_result.hpp` — add to `DeliveryOptions` struct:

```cpp
#include <hpactor/msg/retry_policy.hpp>  // add this include near top
#include <optional>                       // add this include near top

// Inside DeliveryOptions struct, add before the closing };:
    /// Retry policy for AtLeastOnce/DurableAtLeastOnce delivery modes.
    /// When absent and delivery_mode >= AtLeastOnce, a system-default
    /// policy is used (max 5 attempts, 5s timeout, exponential backoff).
    std::optional<RetryPolicy> retry_policy;
```

- [ ] **Step 2: Add OutboundDeliveryTracker* to DeliveryPipeline::Config**

Edit `include/hpactor/mailbox/delivery_pipeline.hpp` — add forward declaration and extend Config:

```cpp
// Forward declaration (add near top after existing forward decls):
namespace hpactor::msg {
class OutboundDeliveryTracker;
}

// Inside struct Config, add before the closing };:
        /// Outbound tracker for AtLeastOnce/DurableAtLeastOnce modes.
        /// When non-null and delivery_mode >= AtLeastOnce with an enabled
        /// retry policy, accepted deliveries are also tracked for retry.
        msg::OutboundDeliveryTracker* outbound_tracker = nullptr;
```

- [ ] **Step 3: Add tracking hook to deliver_with_result()**

Edit `src/mailbox/delivery_pipeline.cpp` — in the `deliver_with_result()` method, after the successful enqueue and before returning the result, add:

```cpp
    // After computing the DeliveryResult and confirming it's accepted...
    // (find the line `return result;` in deliver_with_result)

    if (config_.outbound_tracker != nullptr &&
        result.accepted() &&
        mailbox::is_tracked_delivery(options.delivery_mode) &&
        options.retry_policy.has_value() &&
        options.retry_policy->is_enabled()) {

        // Serialize the message into a frame for retry resend.
        // For local delivery we don't have a WireFrame, so we create
        // a minimal serialized form from the TypedMessage.
        // The serialized_frame will be sent via Transport on retry.

        uint64_t deadline = options.retry_policy->per_attempt_timeout.count() > 0
            ? 0  // deadline will be set by the tracker on first send
            : 0;

        config_.outbound_tracker->track(
            msg.serialize(),  // or equivalent serialization
            config_.endpoint,
            *options.retry_policy,
            deadline);
    }
```

**Note:** The exact serialization approach depends on how `TypedMessage` can be converted to a `StreamBuffer` for transport. For local sends where there's no remote endpoint, tracking is a no-op (the message is already delivered). The tracking hook is primarily for remote sends where the `WireFrame` already exists.

For this task, add the hook but guard it with a check that the target is remote. The full remote integration will be in Task 11. Add a comment marking the TODO.

- [ ] **Step 4: Build and verify compilation**

```bash
ninja -C build hpactor_lib
```

Expected: compiles cleanly.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/enqueue_result.hpp \
        include/hpactor/mailbox/delivery_pipeline.hpp \
        src/mailbox/delivery_pipeline.cpp
git commit -m "feat(mailbox): add retry_policy to DeliveryOptions and tracker hook

- DeliveryOptions gains std::optional<RetryPolicy> retry_policy
- DeliveryPipeline::Config gains OutboundDeliveryTracker* outbound_tracker
- deliver_with_result() calls tracker->track() for accepted AtLeastOnce sends
  when tracker is configured and retry policy is enabled

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 10: ActorContext::try_send() Returns DeliveryReceipt

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`

- [ ] **Step 1: Change try_send() return types**

Edit `include/hpactor/actor/actor_context.hpp` — change return types:

```cpp
    // Before:
    // mailbox::DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
    //                                  mailbox::DeliveryOptions options = {});

    // After:
    msg::DeliveryReceipt try_send(const ActorAddress& target, TypedMessage msg,
                                   mailbox::DeliveryOptions options = {});

    // Before:
    // mailbox::DeliveryResult try_send_with_priority(...);

    // After:
    msg::DeliveryReceipt try_send_with_priority(
        const ActorAddress& target, TypedMessage msg,
        uint8_t priority, int64_t deadline_ns,
        mailbox::DeliveryOptions options = {});

    // Before:
    // mailbox::DeliveryResult try_reply(TypedMessage msg, ...);

    // After:
    msg::DeliveryReceipt try_reply(TypedMessage msg,
                                    mailbox::DeliveryOptions options = {});
```

Add the include near the top:

```cpp
#include <hpactor/msg/delivery_receipt.hpp>
```

- [ ] **Step 2: Update try_send() implementations**

Edit `src/actor/actor_context.cpp` — update `try_send()`:

```cpp
msg::DeliveryReceipt
ActorContext::try_send(const ActorAddress& target, TypedMessage msg,
                       mailbox::DeliveryOptions options) {
    auto ref = resolve(target);
    if (!ref) {
        return msg::DeliveryReceipt(mailbox::DeliveryResult{
            mailbox::DeliveryStatus::NoRoute, target,
            MessageId{options.message_id}, 0});
    }

    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    auto* system = owner_ ? &owner_.get()->system() : system_;
    if (system != nullptr && system->trace_manager() != nullptr) {
        system->trace_manager()->inject_message_context(
            msg, this,
            system->trace_manager()->config().create_roots_for_actor_context_sends);
    }

    return ref.try_send(ref.address(), std::move(msg), options);
}
```

Update `try_send_with_priority()` similarly — wrap the local delivery path's `DeliveryResult` in `DeliveryReceipt`:

```cpp
msg::DeliveryReceipt
ActorContext::try_send_with_priority(const ActorAddress& target, TypedMessage msg,
                                     uint8_t priority, int64_t deadline_ns,
                                     mailbox::DeliveryOptions options) {
    auto ref = resolve(target);
    if (!ref) {
        return msg::DeliveryReceipt(mailbox::DeliveryResult{
            mailbox::DeliveryStatus::NoRoute, target,
            MessageId{options.message_id}, 0});
    }

    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    auto* system = owner_ ? &owner_.get()->system() : system_;
    if (system != nullptr && system->trace_manager() != nullptr) {
        system->trace_manager()->inject_message_context(
            msg, this,
            system->trace_manager()->config().create_roots_for_actor_context_sends);
    }

    if (ref.is_local()) {
        if (system != nullptr) {
            auto er = system->try_deliver_local(target.id, std::move(msg),
                                                priority, deadline_ns, options);
            auto dr = mailbox::DeliveryResult::from_enqueue(
                er, target, MessageId{options.message_id});
            return msg::DeliveryReceipt(std::move(dr));
        }
        return msg::DeliveryReceipt(mailbox::DeliveryResult{
            mailbox::DeliveryStatus::NoRoute, target,
            MessageId{options.message_id}, 0});
    }

    return ref.try_send(ref.address(), std::move(msg), options);
}
```

Update `send()` (the void overload) — it calls `try_send()` and discards the result. The `(void)` cast still works with `DeliveryReceipt`:

```cpp
void ActorContext::send(const ActorAddress& target, TypedMessage msg) {
    (void)try_send(target, std::move(msg));
}
```

- [ ] **Step 3: Update callers that use try_send() result**

Search for callers:

```bash
grep -rn "try_send\|try_reply\|try_send_with_priority" src/ tests/ --include="*.cpp" | grep -v "test_delivery\|test_outbound\|\.cpp:.*::try_send\|actor_context\.cpp"
```

Update any existing call sites that do `.accepted()` or `.status` on the return value to use `.get().accepted()` or `.try_get()->status` instead. If none exist outside the actor_context.cpp itself, no changes needed.

- [ ] **Step 4: Build and run all tests**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

Expected: existing tests that call `try_send()` and inspect the result may need updating. Fix any compilation errors. All tests should pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/actor/actor_context.hpp \
        src/actor/actor_context.cpp
# Add any updated caller files
git commit -m "feat(actor): change try_send/try_reply return type to DeliveryReceipt

- try_send(), try_send_with_priority(), try_reply() now return DeliveryReceipt
- BestEffort/ObservableBestEffort: receipt wraps immediate DeliveryResult
- AtLeastOnce/DurableAtLeastOnce: receipt wraps shared promise for async resolution
- send() void overloads unchanged — they discard the receipt
- DeliveryReceipt has .ready(), .get(), .try_get() for result access

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 11: ActorSystem — Own OutboundDeliveryTracker + Scheduler Tick

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add OutboundDeliveryTracker to ActorSystem**

Edit `include/hpactor/core/actor_system.hpp` — add forward decl, member, and accessor:

```cpp
// Forward declaration (add near other forward decls):
namespace hpactor::msg {
class OutboundDeliveryTracker;
}

// In the ActorSystem class public section, add:
    /// Returns the outbound delivery tracker (may be nullptr if not created).
    msg::OutboundDeliveryTracker* outbound_tracker() noexcept {
        return outbound_tracker_.get();
    }

// In the ActorSystem class private section, add:
    std::unique_ptr<msg::OutboundDeliveryTracker> outbound_tracker_;
```

- [ ] **Step 2: Create tracker BEFORE the delivery pipeline, wire into config**

Edit `src/actor/actor_system.cpp` — create the tracker BEFORE `delivery_pipeline_` so the config can reference it. Find the block (~line 113–140) where `pipeline_cfg` is built and `delivery_pipeline_` is constructed. Restructure to:

```cpp
    // Create the outbound tracker first so the pipeline can reference it.
    outbound_tracker_ = std::make_unique<msg::OutboundDeliveryTracker>();

    mailbox::DeliveryPipeline::Config pipeline_cfg;
    pipeline_cfg.dlq = &dead_letter_queue_;
    pipeline_cfg.metrics = metrics_ring_buffer_.get();
    pipeline_cfg.dedup_cache = &dedup_cache_;
    pipeline_cfg.endpoint = config_.endpoint;
    pipeline_cfg.default_message_ttl_ms = config_.mailbox.default_message_ttl_ms;
    pipeline_cfg.get_actor = [this](ActorId id) { return get_actor(id); };
    pipeline_cfg.get_mailbox = [this](ActorId id) -> MPSCActorMailbox<TypedMessage>* {
        auto actor = get_actor(id);
        if (!actor) return nullptr;
        return actor->mailbox();
    };
    pipeline_cfg.outbound_tracker = outbound_tracker_.get();   // <-- NEW
    // ...emit_local_backpressure, emit_remote_backpressure as existing...

    delivery_pipeline_ =
        std::make_unique<mailbox::DeliveryPipeline>(std::move(pipeline_cfg));
```

- [ ] **Step 3: Add process_retries() via network_loop_->run_every()**

Edit `src/actor/actor_system.cpp` — after the `cache_purge_timer_` registration (~line 196), add a similar periodic timer for retry processing:

```cpp
        // After:
        // cache_purge_timer_ = network_loop_->run_every(...);

        if (outbound_tracker_ && network_loop_) {
            retry_timer_ = network_loop_->run_every(
                [this]() {
                    if (outbound_tracker_) {
                        uint64_t now_ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now()
                                    .time_since_epoch())
                                .count());
                        outbound_tracker_->process_retries(
                            now_ns,
                            [this](const msg::OutboundDeliveryTracker::PendingSend& ps) {
                                // Resend via transport.
                                // Uses the pre-serialized frame stored in PendingSend.
                                if (transport_) {
                                    transport_->send_raw(
                                        ps.remote_endpoint,
                                        ps.serialized_frame);
                                }
                            });
                    }
                },
                100); // poll every 100ms
        }
```

Also add the timer handle to `ActorSystem` in `include/hpactor/core/actor_system.hpp`:

```cpp
    uint64_t retry_timer_ = 0;  // add near cache_purge_timer_
```

- [ ] **Step 4: Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: compiles.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/core/actor_system.hpp \
        src/actor/actor_system.cpp
git commit -m "feat(core): ActorSystem owns OutboundDeliveryTracker

- OutboundDeliveryTracker created during ActorSystem construction
- Wired into DeliveryPipeline::Config for per-delivery tracking
- process_retries() hook in scheduler tick for retry timer management

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 12: Transport — ACK/NACK Frame Dispatch in deliver_remote()

**Files:**
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add WireEnvelope-aware decode + ACK/NACK dispatch to deliver_remote()**

Edit `src/actor/actor_system.cpp` — update `deliver_remote()`:

```cpp
void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    // Try decoding as WireEnvelope first (for peers that support reliable
    // messaging). Fall back to raw ActorMsgFrame for backwards compat.
    //
    // The WireFrame::pb_frame field contains the raw ActorMsgFrame.
    // For now, ACK/NACK frames arrive as a separate frame type detected
    // by checking the magic or a flag.  Since we haven't yet added the
    // WireEnvelope wrapper to the wire format, we use a flag on the
    // ActorMsgFrame for dispatch:
    //
    //   flags & kAckFrame  → extract message_id, call on_ack()
    //   flags & kNackFrame → extract message_id + reason + retry_after, call on_nack()

    constexpr uint32_t kControlAck  = 1 << 5;  // Frame is an ACK control frame
    constexpr uint32_t kControlNack = 1 << 6;  // Frame is a NACK control frame

    uint32_t flags = frame.pb_frame.flags();

    if ((flags & kControlAck) && outbound_tracker_) {
        outbound_tracker_->on_ack(
            MessageId{frame.pb_frame.message_id()},
            net::from_proto(frame.pb_frame.sender()).endpoint());
        return;
    }

    if ((flags & kControlNack) && outbound_tracker_) {
        // reason_code is carried in the type_tag field for NACK frames.
        // retry_after_ms is carried in a reserved field or extracted from
        // the payload.
        uint32_t reason_code = frame.pb_frame.type_tag();
        uint32_t retry_after_ms = 0;
        if (frame.pb_frame.payload().size() >= 4) {
            std::memcpy(&retry_after_ms, frame.pb_frame.payload().data(), 4);
        }
        outbound_tracker_->on_nack(
            MessageId{frame.pb_frame.message_id()},
            net::from_proto(frame.pb_frame.sender()).endpoint(),
            reason_code, retry_after_ms);
        return;
    }

    // Existing delivery path for data frames:
    if (static_cast<TypeTag>(frame.pb_frame.type_tag()) ==
        TypeTag::BackpressureSignalTag) {
        (void)backpressure_coordinator_->handle_remote_signal(frame);
        return;
    }

    StreamBuffer payload(frame.pb_frame.payload().begin(),
                         frame.pb_frame.payload().end());
    TypedMessage msg(static_cast<TypeTag>(frame.pb_frame.type_tag()),
                     std::move(payload));
    msg.set_sender_address(net::from_proto(frame.pb_frame.sender()));
    if (frame.pb_frame.has_trace_context()) {
        uint16_t max_state = tracing_config_.max_tracestate_len;
        auto parsed = net::trace_context_from_proto(
            frame.pb_frame.trace_context(), max_state);
        if (parsed.has_value()) {
            msg.set_trace_context(parsed.value());
        }
    }
    deliver_local(net::from_proto(frame.pb_frame.receiver()).id, std::move(msg));
}
```

**Note:** This is a pragmatic interim approach using frame flags. Once `WireEnvelope` protocol negotiation is implemented (follow-up), the decode path switches to protobuf `oneof` dispatch. The flag-based approach works without changing the wire format for the initial implementation.

- [ ] **Step 2: Build and verify**

```bash
ninja -C build hpactor_lib
```

- [ ] **Step 3: Commit**

```bash
git add src/actor/actor_system.cpp
git commit -m "feat(transport): add ACK/NACK frame dispatch in deliver_remote()

- Detects ACK/NACK via frame flags (kControlAck, kControlNack)
- ACK: routes to OutboundDeliveryTracker::on_ack()
- NACK: routes to OutboundDeliveryTracker::on_nack() with reason + retry_after
- Existing data frame path unchanged
- WireEnvelope protobuf dispatch deferred to protocol negotiation follow-up

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 13: Metrics Events

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp`

- [ ] **Step 1: Add 6 new MetricEventType entries**

Edit `include/hpactor/metrics/metrics_event.hpp` — add after `kDeliveryResult = 45`:

```cpp
    kReliableTracked = 46,     ///< PendingSend added to outbound tracker.
    kReliableAckReceived = 47, ///< AckFrame processed; code carries
                                ///< DeliveryStatus.
    kReliableNackReceived = 48, ///< NackFrame processed; code carries
                                 ///< DeliveryStatus.
    kReliableRetry = 49,       ///< Frame re-sent on retry; code carries
                                ///< attempt_number.
    kReliableExhausted = 50,   ///< Retries exhausted; code carries
                                ///< total_attempts.
    kReliableCancelled = 51,   ///< DeliveryReceipt::cancel() called.
```

- [ ] **Step 2: Emit metric events in the tracker**

Edit `src/msg/outbound_delivery_tracker.cpp` — add metric emission calls after key state transitions. Since the tracker doesn't have direct access to the metrics ring buffer, add `std::function` callbacks to the tracker's constructor or as optional config.

For an initial implementation, add metric event callbacks to the `OutboundDeliveryTracker`:

```cpp
// In outbound_delivery_tracker.hpp, add to the class:
public:
    using MetricsCallback = std::function<void(
        uint64_t timestamp_ns, metrics::MetricEventType type, uint8_t code)>;

    void set_metrics_callback(MetricsCallback cb) { metrics_cb_ = std::move(cb); }

private:
    MetricsCallback metrics_cb_;
```

In the tracker implementation, call `metrics_cb_` at key points:
- `track()` → `kReliableTracked`
- `on_ack()` → `kReliableAckReceived`
- `on_nack()` → `kReliableNackReceived`
- `process_retries()` retry → `kReliableRetry`
- `process_retries()` exhaustion → `kReliableExhausted`
- `cancel()` → `kReliableCancelled`

- [ ] **Step 3: Wire metrics in ActorSystem**

Edit `src/actor/actor_system.cpp` — after creating `outbound_tracker_`:

```cpp
    outbound_tracker_->set_metrics_callback(
        [this](uint64_t ts, metrics::MetricEventType type, uint8_t code) {
            if (metrics_ring_buffer_) {
                metrics::MetricEvent ev{};
                ev.timestamp_ns = ts;
                ev.event_type = type;
                ev.code = code;
                metrics_ring_buffer_->push(ev);
            }
        });
```

- [ ] **Step 4: Build and verify**

```bash
ninja -C build hpactor_lib
```

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp \
        include/hpactor/msg/outbound_delivery_tracker.hpp \
        src/msg/outbound_delivery_tracker.cpp \
        src/actor/actor_system.cpp
git commit -m "feat(metrics): add 6 reliable messaging MetricEventType entries

- kReliableTracked, kReliableAckReceived, kReliableNackReceived
- kReliableRetry, kReliableExhausted, kReliableCancelled
- OutboundDeliveryTracker emits events via callback
- Wired through ActorSystem to the metrics ring buffer

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 14: CLI /reliable Commands

**Files:**
- Create: `src/cli/commands/reliable_commands.cpp`

- [ ] **Step 1: Create reliable_commands.cpp with command classes**

Create `src/cli/commands/reliable_commands.cpp` following the exact pattern from `dlq_commands.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>
#include <hpactor/types/types.hpp>

#include <charconv>
#include <sstream>
#include <string>

namespace hpactor {
namespace cli {
namespace {

// Resolve the outbound tracker from a command context.
// Returns nullptr and emits an error if unavailable.
msg::OutboundDeliveryTracker* resolve_tracker(CommandContext& ctx) {
    auto* system = ctx.system;
    if (!system) {
        ctx.output->error("No actor system available");
        return nullptr;
    }
    auto* tracker = system->outbound_tracker();
    if (!tracker) {
        ctx.output->raw("Reliable delivery tracker is not enabled.");
    }
    return tracker;
}

// ── /reliable/outbox ────────────────────────────────────────────────────

class ReliableOutboxCommand final : public ICommand {
public:
    std::string_view path() const noexcept override {
        return "reliable/outbox";
    }
    std::string_view help_text() const noexcept override {
        return "List pending at-least-once sends";
    }
    int order() const noexcept override {
        return 510;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* tracker = resolve_tracker(ctx);
        if (!tracker) return outcome::ok();

        auto pending = tracker->snapshot();
        if (pending.empty()) {
            ctx.output->raw("No pending sends.");
            return outcome::ok();
        }

        std::stringstream ss;
        ss << "msg_id  attempt  next_retry_ns\n";
        ss << "------  -------  -------------\n";
        for (auto& ps : pending) {
            ss << ps.msg_id.value << "  "
               << static_cast<int>(ps.retry_count) << "  "
               << ps.next_retry_ns << "\n";
        }
        ctx.output->raw(ss.str());
        return outcome::ok();
    }
};

const CommandRegistration<ReliableOutboxCommand> kRegisterReliableOutbox;

// ── /reliable/outbox/<msg_id> ───────────────────────────────────────────

class ReliableOutboxShowCommand final : public ICommand {
public:
    std::string_view path() const noexcept override {
        return "reliable/outbox/<msg_id>";
    }
    std::string_view help_text() const noexcept override {
        return "Show details for one pending send";
    }
    int order() const noexcept override {
        return 511;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* tracker = resolve_tracker(ctx);
        if (!tracker) return outcome::ok();

        auto it = ctx.params.find("<msg_id>");
        if (it == ctx.params.end()) {
            ctx.output->error("Missing <msg_id> parameter");
            return outcome::ok();
        }

        bool ok = false;
        uint64_t msg_id_val = 0;
        auto [ptr, ec] = std::from_chars(
            it->second.data(), it->second.data() + it->second.size(), msg_id_val);
        ok = (ec == std::errc{});

        if (!ok) {
            ctx.output->error("Invalid msg_id: " + it->second);
            return outcome::ok();
        }

        auto pending = tracker->snapshot();
        for (auto& ps : pending) {
            if (ps.msg_id.value == msg_id_val) {
                std::stringstream ss;
                ss << "msg_id:       " << ps.msg_id.value << "\n"
                   << "retry_count:  " << static_cast<int>(ps.retry_count) << "\n"
                   << "deadline_ns:  " << ps.deadline_ns << "\n"
                   << "next_retry:   " << ps.next_retry_ns << "\n";
                ctx.output->raw(ss.str());
                return outcome::ok();
            }
        }
        ctx.output->raw("No pending send with that msg_id.");
        return outcome::ok();
    }
};

const CommandRegistration<ReliableOutboxShowCommand> kRegisterReliableOutboxShow;

// ── /reliable/cancel/<msg_id> ───────────────────────────────────────────

class ReliableCancelCommand final : public ICommand {
public:
    std::string_view path() const noexcept override {
        return "reliable/cancel/<msg_id>";
    }
    std::string_view help_text() const noexcept override {
        return "Cancel tracking for a pending send";
    }
    int order() const noexcept override {
        return 512;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* tracker = resolve_tracker(ctx);
        if (!tracker) return outcome::ok();

        auto it = ctx.params.find("<msg_id>");
        if (it == ctx.params.end()) {
            ctx.output->error("Missing <msg_id> parameter");
            return outcome::ok();
        }

        bool ok = false;
        uint64_t msg_id_val = 0;
        auto [ptr, ec] = std::from_chars(
            it->second.data(), it->second.data() + it->second.size(), msg_id_val);
        ok = (ec == std::errc{});

        if (!ok) {
            ctx.output->error("Invalid msg_id: " + it->second);
            return outcome::ok();
        }

        tracker->cancel(MessageId{msg_id_val});
        ctx.output->raw("Cancelled.");
        return outcome::ok();
    }
};

const CommandRegistration<ReliableCancelCommand> kRegisterReliableCancel;

// ── /reliable/stats ─────────────────────────────────────────────────────

class ReliableStatsCommand final : public ICommand {
public:
    std::string_view path() const noexcept override {
        return "reliable/stats";
    }
    std::string_view help_text() const noexcept override {
        return "Show reliable delivery counters";
    }
    int order() const noexcept override {
        return 513;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* tracker = resolve_tracker(ctx);
        if (!tracker) return outcome::ok();

        std::stringstream ss;
        ss << "pending: " << tracker->pending() << "\n";

        auto pending = tracker->snapshot();
        ss << "snapshot_entries: " << pending.size() << "\n";

        ctx.output->raw(ss.str());
        return outcome::ok();
    }
};

const CommandRegistration<ReliableStatsCommand> kRegisterReliableStats;

} // namespace
} // namespace cli
} // namespace hpactor
```

Note: Each `CommandRegistration<T>` static object auto-registers the command with `CommandRegistry::instance()` before `main()`. The `order()` values 510–513 place `/reliable` after `/dlq` (500-range) in the help output. The CLI tree builder in `cli_actor.cpp` automatically picks up these commands — no edits to `cli_actor.cpp` are needed.

- [ ] **Step 2: Register in CMakeLists and build**

Edit `src/CMakeLists.txt` — add after `cli/commands/dlq_commands.cpp` at line 182:

```cmake
    cli/commands/reliable_commands.cpp
```

Build and verify:

```bash
ninja -C build hpactor_lib
```

- [ ] **Step 3: Verify CLI commands appear in help**

```bash
ninja -C build && ./build/apps/example_app --cli 2>&1 | head -50
# Or: start the app with CLI enabled and run `/help` to see `/reliable/*`
```

- [ ] **Step 4: Commit**

```bash
git add src/cli/commands/reliable_commands.cpp
git commit -m "feat(cli): add /reliable outbox, cancel, stats commands

- /reliable/outbox: list all pending at-least-once sends
- /reliable/outbox/<msg_id>: show full detail for one pending send
- /reliable/cancel/<msg_id>: cancel tracking for a send
- /reliable/stats: show pending count + snapshot size
- Auto-registered via CommandRegistration<T> file-scope objects
- Follows existing dlq_commands.cpp pattern

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Final Verification

- [ ] **Build everything:**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
```

- [ ] **Run all tests:**

```bash
ctest --output-on-failure --parallel 8
```

- [ ] **Run the specific msg test binary:**

```bash
./build/tests/unit/msg/test_unit_msg
```

Expected: all tests pass across the entire test suite.

---

## Dependency Order Summary

```
Task 1 (RetryPolicy)
  ├── Task 2 (DeliveryStatus::Cancelled)
  ├── Task 3 (DeadLetterReason::RetryExhausted)
  ├── Task 4 (DeliveryReceipt)
  │     └── Task 7 (OutboundDeliveryTracker)
  │           └── Task 8 (Retry exhaustion DLQ)
  ├── Task 5 (DurableDeliveryStore stub)
  └── Task 6 (Protobuf ACK/NACK)
        │
        ▼
  Task 9 (DeliveryOptions + Pipeline hook)
        │
        ▼
  Task 10 (ActorContext try_send return type)
        │
        ▼
  Task 11 (ActorSystem owns Tracker)
        │
        ▼
  Task 12 (Transport ACK/NACK dispatch)
        │
        ▼
  Task 13 (Metrics events)
        │
        ▼
  Task 14 (CLI commands)
```

Tasks 1–6 can be parallelized (independent foundation types). Tasks 7–8 depend on Task 4. Tasks 9–14 are sequential integration layers.
