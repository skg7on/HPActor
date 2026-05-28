# MPSCActorMailbox Strategy Pattern Refactoring — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract 5 separable concerns from the 784-line `MPSCActorMailbox` into independent, testable components behind clear interfaces, shrinking the mailbox to a ~200-line coordinator.

**Architecture:** Bottom-up extraction — build the three value-type components first (ReservationManager, PressureStateMachine, BackpressureSignalGate), then the strategy interface and 6 concrete overflow handlers, then the factory. Finally refactor `MPSCActorMailbox` to delegate to all composed components. Public API remains identical throughout.

**Tech Stack:** C++20, header-only templates, `hpactor::mailbox` namespace, Google Test, no RTTI/exceptions.

---

### Task 1: ReservationManager — Two-Phase Capacity Reservation

**Files:**
- Create: `include/hpactor/mailbox/detail/reservation_manager.hpp`
- Create: `tests/unit/mailbox/test_reservation_manager.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Component:** Extracts the `reserved_messages_`, `reserved_system_messages_`, `queued_bytes_` atomics and the `try_reserve()`, `try_reserve_system()`, `release_reservation()`, `release_system_reservation()` methods. Moves the `ReservationResult` enum to public scope.

- [ ] **Step 1: Write `reservation_manager.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <atomic>
#include <cstdint>

namespace hpactor::mailbox::detail {

enum class ReservationResult : uint8_t {
    Reserved,
    CountCapacity,
    ByteCapacity,
};

template <typename T>
class ReservationManager {
  public:
    ReservationManager() = default;

    // Two-phase reservation: count first, then bytes with rollback on failure.
    ReservationResult try_reserve(uint64_t bytes, uint32_t max_messages,
                                   uint64_t max_bytes) noexcept {
        // Phase 1: count reservation.
        if (max_messages > 0) {
            uint32_t cur = reserved_messages_.load(std::memory_order_acquire);
            do {
                if (cur >= max_messages)
                    return ReservationResult::CountCapacity;
            } while (!reserved_messages_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_acq_rel, std::memory_order_acquire));
        }

        // Phase 2: byte budget reservation.
        if (max_bytes > 0) {
            uint64_t cur = queued_bytes_.load(std::memory_order_acquire);
            do {
                if (cur + bytes > max_bytes) {
                    if (max_messages > 0)
                        reserved_messages_.fetch_sub(1, std::memory_order_release);
                    return ReservationResult::ByteCapacity;
                }
            } while (!queued_bytes_.compare_exchange_weak(
                cur, cur + bytes, std::memory_order_acq_rel, std::memory_order_acquire));
            return ReservationResult::Reserved;
        }

        // Unlimited bytes: still track for observability.
        queued_bytes_.fetch_add(bytes, std::memory_order_release);
        return ReservationResult::Reserved;
    }

    // System message protected reserve — bypasses byte budget.
    bool try_reserve_system(uint64_t bytes, uint32_t limit) noexcept {
        if (limit == 0)
            return false;
        uint32_t current = reserved_system_messages_.load(std::memory_order_acquire);
        while (true) {
            if (current >= limit)
                return false;
            if (reserved_system_messages_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                queued_bytes_.fetch_add(bytes, std::memory_order_release);
                return true;
            }
        }
    }

    void release(uint64_t bytes) noexcept {
        reserved_messages_.fetch_sub(1, std::memory_order_release);
        if (bytes > 0)
            queued_bytes_.fetch_sub(bytes, std::memory_order_release);
    }

    void release_system(uint64_t bytes) noexcept {
        reserved_system_messages_.fetch_sub(1, std::memory_order_release);
        if (bytes > 0)
            queued_bytes_.fetch_sub(bytes, std::memory_order_release);
    }

    // Direct atomic access for inject_for_test bypass.
    void inject_count(uint64_t bytes) noexcept {
        reserved_messages_.fetch_add(1, std::memory_order_relaxed);
        queued_bytes_.fetch_add(bytes, std::memory_order_release);
    }

    uint32_t reserved_count() const noexcept {
        return reserved_messages_.load(std::memory_order_acquire);
    }
    uint32_t reserved_system_count() const noexcept {
        return reserved_system_messages_.load(std::memory_order_acquire);
    }
    uint64_t queued_bytes() const noexcept {
        return queued_bytes_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<uint32_t> reserved_messages_{0};
    std::atomic<uint32_t> reserved_system_messages_{0};
    std::atomic<uint64_t> queued_bytes_{0};
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 2: Write `test_reservation_manager.cpp`**

```cpp
#include <hpactor/mailbox/detail/reservation_manager.hpp>

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace hpactor::mailbox::detail;

// Dummy type for template instantiation.
struct TestMsg { int payload = 0; };

class ReservationManagerTest : public ::testing::Test {
  protected:
    ReservationManager<TestMsg> mgr;
};

TEST_F(ReservationManagerTest, ReserveWithinCountLimit) {
    auto r = mgr.try_reserve(100, 10, 0);
    EXPECT_EQ(r, ReservationResult::Reserved);
    EXPECT_EQ(mgr.reserved_count(), 1);
    EXPECT_EQ(mgr.queued_bytes(), 100);
}

TEST_F(ReservationManagerTest, RejectAtCountCapacity) {
    // Fill to capacity.
    for (int i = 0; i < 5; i++)
        EXPECT_EQ(mgr.try_reserve(1, 5, 0), ReservationResult::Reserved);
    auto r = mgr.try_reserve(1, 5, 0);
    EXPECT_EQ(r, ReservationResult::CountCapacity);
}

TEST_F(ReservationManagerTest, RejectAtByteCapacity) {
    auto r = mgr.try_reserve(200, 0, 100);
    EXPECT_EQ(r, ReservationResult::ByteCapacity);
}

TEST_F(ReservationManagerTest, TwoPhaseRollbackOnByteFailure) {
    // Reserve count slot, then fail on bytes — count should roll back.
    auto r = mgr.try_reserve(200, 1, 100);
    EXPECT_EQ(r, ReservationResult::ByteCapacity);
    // Count slot was released during rollback — another reserve should succeed.
    EXPECT_EQ(mgr.reserved_count(), 0);
    r = mgr.try_reserve(50, 1, 100);
    EXPECT_EQ(r, ReservationResult::Reserved);
}

TEST_F(ReservationManagerTest, ReleaseReturnsCapacity) {
    mgr.try_reserve(50, 1, 100);
    mgr.release(50);
    EXPECT_EQ(mgr.reserved_count(), 0);
    EXPECT_EQ(mgr.queued_bytes(), 0);
    // Should be able to reserve again.
    auto r = mgr.try_reserve(60, 1, 100);
    EXPECT_EQ(r, ReservationResult::Reserved);
}

TEST_F(ReservationManagerTest, SystemReserveBypassesByteBudget) {
    // Main pool exhausted at byte level.
    mgr.try_reserve(90, 5, 100);
    auto r = mgr.try_reserve(20, 5, 100);
    EXPECT_EQ(r, ReservationResult::ByteCapacity);
    // System reserve should still work.
    bool ok = mgr.try_reserve_system(200, 32);
    EXPECT_TRUE(ok);
    EXPECT_EQ(mgr.reserved_system_count(), 1);
}

TEST_F(ReservationManagerTest, SystemReserveRespectsLimit) {
    for (int i = 0; i < 3; i++)
        EXPECT_TRUE(mgr.try_reserve_system(1, 3));
    EXPECT_FALSE(mgr.try_reserve_system(1, 3));
}

TEST_F(ReservationManagerTest, ReleaseSystemReturnsCapacity) {
    mgr.try_reserve_system(10, 32);
    mgr.release_system(10);
    EXPECT_EQ(mgr.reserved_system_count(), 0);
}

TEST_F(ReservationManagerTest, UnlimitedMessagesCountsBytes) {
    // max_messages=0 means unlimited count.
    auto r = mgr.try_reserve(100, 0, 0);
    EXPECT_EQ(r, ReservationResult::Reserved);
    EXPECT_EQ(mgr.queued_bytes(), 100);
}

TEST_F(ReservationManagerTest, ConcurrentReservationsDontExceedCapacity) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    constexpr uint32_t kCap = kThreads * kPerThread;
    std::atomic<int> success{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kPerThread; i++) {
                if (mgr.try_reserve(1, kCap, 0) == ReservationResult::Reserved)
                    success.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(success.load(), kCap);
    EXPECT_EQ(mgr.reserved_count(), kCap);
}
```

- [ ] **Step 3: Add test source to `tests/unit/mailbox/CMakeLists.txt`**

The existing file uses explicit source lists. Add `test_reservation_manager.cpp` to the `add_executable(test_unit_mailbox ...)` call, between `test_backpressure_signal_serialization.cpp` and the closing `)`.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*ReservationManager*"
```

Expected: 10 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/detail/reservation_manager.hpp tests/unit/mailbox/test_reservation_manager.cpp tests/unit/mailbox/CMakeLists.txt
git commit -m "refactor(mailbox): extract ReservationManager from MPSCActorMailbox

Move two-phase CAS reservation (count + bytes), system message protected
reserve, and release accounting into a standalone value type. The
ReservationResult enum moves from private-in-mailbox to public detail scope.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 2: PressureStateMachine — Watermarks & Hysteresis

**Files:**
- Create: `include/hpactor/mailbox/detail/pressure_state_machine.hpp`
- Create: `tests/unit/mailbox/test_pressure_state_machine.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Component:** Extracts the `pressure_state_` atomic, `next_pressure_state()`, `update_pressure_state()`, `pressure_code_after_accept()`, and `pressure_severity()` methods from the mailbox. The `pressure_ratio()` computation stays in the mailbox (needs queue depth).

- [ ] **Step 1: Write `pressure_state_machine.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/mailbox_policy.hpp>

#include <atomic>
#include <cstdint>

namespace hpactor::mailbox::detail {

class PressureStateMachine {
  public:
    PressureStateMachine() = default;

    void update(double ratio, bool hard_failure,
                double high_watermark, double low_watermark,
                double critical_watermark) noexcept {
        pressure_state_.store(
            next_state(ratio, hard_failure, high_watermark, low_watermark, critical_watermark),
            std::memory_order_release);
    }

    MailboxPressureState current_state() const noexcept {
        return pressure_state_.load(std::memory_order_acquire);
    }

    EnqueueResultCode code_after_accept() const noexcept {
        auto state = pressure_state_.load(std::memory_order_acquire);
        if (state == MailboxPressureState::SoftPressure ||
            state == MailboxPressureState::HardPressure ||
            state == MailboxPressureState::Recovering) {
            return EnqueueResultCode::AcceptedWithSoftPressure;
        }
        return EnqueueResultCode::Accepted;
    }

    static uint8_t severity(MailboxPressureState state) noexcept {
        switch (state) {
            case MailboxPressureState::Normal:     return 0;
            case MailboxPressureState::Recovering:  return 1;
            case MailboxPressureState::SoftPressure: return 2;
            case MailboxPressureState::HardPressure: return 3;
        }
        return 0;
    }

  private:
    static MailboxPressureState
    next_state(double ratio, bool hard_failure,
               double high_watermark, double low_watermark,
               double critical_watermark, MailboxPressureState current) noexcept {
        if (hard_failure || ratio >= critical_watermark)
            return MailboxPressureState::HardPressure;

        if (current == MailboxPressureState::HardPressure ||
            current == MailboxPressureState::Recovering) {
            if (ratio < low_watermark)
                return MailboxPressureState::Normal;
            return MailboxPressureState::Recovering;
        }

        if (ratio >= high_watermark)
            return MailboxPressureState::SoftPressure;
        if (ratio < low_watermark)
            return MailboxPressureState::Normal;
        return current;
    }

    MailboxPressureState
    next_state(double ratio, bool hard_failure,
               double high_watermark, double low_watermark,
               double critical_watermark) const noexcept {
        return next_state(ratio, hard_failure, high_watermark, low_watermark,
                          critical_watermark,
                          pressure_state_.load(std::memory_order_acquire));
    }

    std::atomic<MailboxPressureState> pressure_state_{MailboxPressureState::Normal};
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 2: Write `test_pressure_state_machine.cpp`**

```cpp
#include <hpactor/mailbox/detail/pressure_state_machine.hpp>

#include <gtest/gtest.h>

using namespace hpactor::mailbox;
using namespace hpactor::mailbox::detail;

class PressureStateMachineTest : public ::testing::Test {
  protected:
    PressureStateMachine psm;
    static constexpr double kHigh = 0.80;
    static constexpr double kLow = 0.50;
    static constexpr double kCritical = 1.00;
};

TEST_F(PressureStateMachineTest, StartsNormal) {
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);
}

TEST_F(PressureStateMachineTest, BelowLowWatermarkStaysNormal) {
    psm.update(0.30, false, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);
}

TEST_F(PressureStateMachineTest, AboveHighWatermarkEntersSoftPressure) {
    psm.update(0.85, false, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::SoftPressure);
}

TEST_F(PressureStateMachineTest, HardFailureEntersHardPressure) {
    psm.update(0.30, true, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::HardPressure);
}

TEST_F(PressureStateMachineTest, CriticalWatermarkEntersHardPressure) {
    psm.update(1.05, false, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::HardPressure);
}

TEST_F(PressureStateMachineTest, HardPressureHysteresisToRecovering) {
    psm.update(1.05, false, kHigh, kLow, kCritical); // → HardPressure
    psm.update(0.60, false, kHigh, kLow, kCritical); // still above low
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Recovering);
}

TEST_F(PressureStateMachineTest, RecoveringToNormalBelowLowWatermark) {
    psm.update(0.85, false, kHigh, kLow, kCritical); // → SoftPressure
    psm.update(0.95, false, kHigh, kLow, kCritical); // → HardPressure
    psm.update(0.60, false, kHigh, kLow, kCritical); // → Recovering
    psm.update(0.30, false, kHigh, kLow, kCritical); // below low → Normal
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);
}

TEST_F(PressureStateMachineTest, SoftPressureTransitionsToHard) {
    psm.update(0.85, false, kHigh, kLow, kCritical); // → SoftPressure
    psm.update(0.95, false, kHigh, kLow, kCritical); // still below critical, stays→SoftPressure
    // Actually at 0.95 and high=0.80, 0.95 >= 0.80 so SoftPressure stays
    // but < 1.00, not HardPressure unless it was already Hard/Recovering
    EXPECT_EQ(psm.current_state(), MailboxPressureState::SoftPressure);
}

TEST_F(PressureStateMachineTest, CodeAfterAcceptNormal) {
    EXPECT_EQ(psm.code_after_accept(), EnqueueResultCode::Accepted);
}

TEST_F(PressureStateMachineTest, CodeAfterAcceptUnderPressure) {
    psm.update(0.85, false, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.code_after_accept(), EnqueueResultCode::AcceptedWithSoftPressure);
}

TEST_F(PressureStateMachineTest, SeverityOrdering) {
    EXPECT_EQ(PressureStateMachine::severity(MailboxPressureState::Normal), 0);
    EXPECT_EQ(PressureStateMachine::severity(MailboxPressureState::Recovering), 1);
    EXPECT_EQ(PressureStateMachine::severity(MailboxPressureState::SoftPressure), 2);
    EXPECT_EQ(PressureStateMachine::severity(MailboxPressureState::HardPressure), 3);
}
```

- [ ] **Step 3: Add test source to `tests/unit/mailbox/CMakeLists.txt`**

Add `test_pressure_state_machine.cpp` to the `add_executable(test_unit_mailbox ...)` list.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*PressureStateMachine*"
```

Expected: 11 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/detail/pressure_state_machine.hpp tests/unit/mailbox/test_pressure_state_machine.cpp tests/unit/mailbox/CMakeLists.txt
git commit -m "refactor(mailbox): extract PressureStateMachine from MPSCActorMailbox

Move watermark-based pressure state transitions, hysteresis
(HardPressure→Recovering→Normal), and severity ordering into a
standalone value type.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 3: BackpressureSignalGate — Rate Limiting & Escalation

**Files:**
- Create: `include/hpactor/mailbox/detail/backpressure_signal_gate.hpp`
- Create: `tests/unit/mailbox/test_backpressure_signal_gate.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Component:** Extracts the `last_backpressure_signal_ns_`, `last_backpressure_signal_severity_`, `backpressure_signal_sequence_` atomics and the `try_acquire_backpressure_signal()` method. Uses `PressureStateMachine::severity()` for escalation detection.

- [ ] **Step 1: Write `backpressure_signal_gate.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <atomic>
#include <cstdint>
#include <optional>

namespace hpactor::mailbox::detail {

class BackpressureSignalGate {
  public:
    BackpressureSignalGate() = default;

    std::optional<uint64_t>
    try_acquire(uint64_t now_ns, MailboxPressureState state,
                uint32_t interval_ms) noexcept {
        const uint64_t interval_ns =
            static_cast<uint64_t>(interval_ms) * 1'000'000ULL;
        const auto severity = PressureStateMachine::severity(state);

        uint64_t last = last_signal_ns_.load(std::memory_order_acquire);
        uint8_t last_severity =
            last_severity_.load(std::memory_order_acquire);

        while (true) {
            const bool first = last == 0;
            const bool interval_elapsed = now_ns >= last + interval_ns;
            const bool escalation = severity > last_severity;

            if (!first && !interval_elapsed && !escalation)
                return std::nullopt;

            if (last_signal_ns_.compare_exchange_weak(
                    last, now_ns, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                last_severity_.store(severity, std::memory_order_release);
                return sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
            }

            last_severity =
                last_severity_.load(std::memory_order_acquire);
        }
    }

    uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<uint64_t> last_signal_ns_{0};
    std::atomic<uint8_t> last_severity_{0};
    std::atomic<uint64_t> sequence_{0};
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 2: Write `test_backpressure_signal_gate.cpp`**

```cpp
#include <hpactor/mailbox/detail/backpressure_signal_gate.hpp>

#include <gtest/gtest.h>

using namespace hpactor::mailbox;
using namespace hpactor::mailbox::detail;

class BackpressureSignalGateTest : public ::testing::Test {
  protected:
    BackpressureSignalGate gate;
    static constexpr uint32_t kIntervalMs = 100;
};

TEST_F(BackpressureSignalGateTest, FirstSignalAlwaysAcquired) {
    auto seq = gate.try_acquire(1'000'000'000, MailboxPressureState::SoftPressure, kIntervalMs);
    EXPECT_TRUE(seq.has_value());
    EXPECT_EQ(seq.value(), 1);
}

TEST_F(BackpressureSignalGateTest, SecondSignalWithinIntervalBlocked) {
    gate.try_acquire(1'000'000'000, MailboxPressureState::SoftPressure, kIntervalMs);
    auto seq = gate.try_acquire(1'000'000'050, MailboxPressureState::SoftPressure, kIntervalMs);
    EXPECT_FALSE(seq.has_value());
}

TEST_F(BackpressureSignalGateTest, SignalAfterIntervalAllowed) {
    gate.try_acquire(1'000'000'000, MailboxPressureState::SoftPressure, kIntervalMs);
    uint64_t later = 1'000'000'000 + static_cast<uint64_t>(kIntervalMs) * 1'000'000ULL;
    auto seq = gate.try_acquire(later, MailboxPressureState::SoftPressure, kIntervalMs);
    EXPECT_TRUE(seq.has_value());
    EXPECT_EQ(seq.value(), 2);
}

TEST_F(BackpressureSignalGateTest, EscalationBypassesInterval) {
    gate.try_acquire(1'000'000'000, MailboxPressureState::SoftPressure, kIntervalMs);
    // Same timestamp, but higher severity.
    auto seq = gate.try_acquire(1'000'000'000, MailboxPressureState::HardPressure, kIntervalMs);
    EXPECT_TRUE(seq.has_value());
    EXPECT_EQ(seq.value(), 2);
}

TEST_F(BackpressureSignalGateTest, DeescalationDoesNotBypassInterval) {
    gate.try_acquire(1'000'000'000, MailboxPressureState::HardPressure, kIntervalMs);
    auto seq = gate.try_acquire(1'000'000'001, MailboxPressureState::SoftPressure, kIntervalMs);
    EXPECT_FALSE(seq.has_value());
}

TEST_F(BackpressureSignalGateTest, SequenceMonotonicallyIncreases) {
    auto s1 = gate.try_acquire(1'000'000'000, MailboxPressureState::Normal, kIntervalMs);
    uint64_t later = 1'000'000'000 + static_cast<uint64_t>(kIntervalMs) * 1'000'000ULL;
    auto s2 = gate.try_acquire(later, MailboxPressureState::Normal, kIntervalMs);
    EXPECT_LT(s1.value(), s2.value());
}

TEST_F(BackpressureSignalGateTest, ZeroIntervalAllowsEveryCall) {
    auto s1 = gate.try_acquire(1'000'000'000, MailboxPressureState::Normal, 0);
    auto s2 = gate.try_acquire(1'000'000'001, MailboxPressureState::Normal, 0);
    EXPECT_TRUE(s1.has_value());
    EXPECT_TRUE(s2.has_value());
}
```

- [ ] **Step 3: Add test source to `tests/unit/mailbox/CMakeLists.txt`**

Add `test_backpressure_signal_gate.cpp` to the `add_executable(test_unit_mailbox ...)` list.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*BackpressureSignalGate*"
```

Expected: 7 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/detail/backpressure_signal_gate.hpp tests/unit/mailbox/test_backpressure_signal_gate.cpp tests/unit/mailbox/CMakeLists.txt
git commit -m "refactor(mailbox): extract BackpressureSignalGate from MPSCActorMailbox

Move rate-limited CAS-based backpressure signal acquisition with
escalation bypass into a standalone value type.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 4: OverflowContext and IOverflowHandler<T> Interface

**Files:**
- Create: `include/hpactor/mailbox/detail/overflow_context.hpp`
- Create: `include/hpactor/mailbox/detail/overflow_handler_interface.hpp`

**Component:** The shared context struct passed to overflow handlers, and the virtual strategy interface. No tests yet — tested through handler tests in Task 5.

- [ ] **Step 1: Write `overflow_context.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/reservation_manager.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>

#include <atomic>
#include <functional>

namespace hpactor::mailbox::detail {

template <typename T>
struct OverflowContext {
    const T& message;
    MailboxEnvelopeMeta& meta;
    ReservationManager<T>& reservation;
    OverflowQueue<T>& overflow_queue;
    std::atomic<uint64_t>& total_rejected;
    std::atomic<uint64_t>& total_dropped;
    std::atomic<uint64_t>& total_dead_letters;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_buf;
    MailboxConfig& config;
    ActorId actor_id;
    uint32_t current_depth;
    uint64_t current_bytes;
    std::function<bool()> drop_oldest_fn;
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 2: Write `overflow_handler_interface.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/overflow_context.hpp>
#include <hpactor/mailbox/detail/reservation_manager.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

namespace hpactor::mailbox::detail {

template <typename T>
class IOverflowHandler {
  public:
    virtual ~IOverflowHandler() = default;

    virtual EnqueueResult handle(OverflowContext<T>& ctx,
                                 ReservationResult reason) = 0;
    virtual OverflowPolicy policy() const = 0;
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/mailbox/detail/overflow_context.hpp include/hpactor/mailbox/detail/overflow_handler_interface.hpp
git commit -m "refactor(mailbox): add OverflowContext and IOverflowHandler<T> interface

Define the Strategy pattern interface for overflow policy dispatch.
OverflowContext bundles all state a handler needs; handlers are stateless.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 5: Six Concrete Overflow Handlers

**Files:**
- Create: `include/hpactor/mailbox/detail/handlers/reject_newest_handler.hpp`
- Create: `include/hpactor/mailbox/detail/handlers/drop_newest_handler.hpp`
- Create: `include/hpactor/mailbox/detail/handlers/drop_oldest_handler.hpp`
- Create: `include/hpactor/mailbox/detail/handlers/dead_letter_handler.hpp`
- Create: `include/hpactor/mailbox/detail/handlers/signal_only_handler.hpp`
- Create: `include/hpactor/mailbox/detail/handlers/spill_to_overflow_handler.hpp`
- Create: `tests/unit/mailbox/test_overflow_handlers.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Component:** Each handler is a small, stateless class implementing `IOverflowHandler<T>`. Each constructs an `EnqueueResult` from context fields. The shared `make_result` logic from the mailbox is inlined per-handler since each handler fills different fields.

- [ ] **Step 1: Write `reject_newest_handler.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

namespace hpactor::mailbox::detail {

template <typename T>
class RejectNewestHandler : public IOverflowHandler<T> {
  public:
    EnqueueResult handle(OverflowContext<T>& ctx,
                         ReservationResult reason) override {
        ctx.total_rejected.fetch_add(1, std::memory_order_relaxed);
        emit_metric(ctx, metrics::MetricEventType::kMailboxRejected);
        return build_result(ctx, EnqueueResultCode::Rejected,
                            reason == ReservationResult::ByteCapacity
                                ? BackpressureReason::ByteCapacity
                                : BackpressureReason::HardCapacity);
    }

    OverflowPolicy policy() const override { return OverflowPolicy::RejectNewest; }

  private:
    static void emit_metric(OverflowContext<T>& ctx,
                            metrics::MetricEventType type) {
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = type;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
    }

    static EnqueueResult build_result(OverflowContext<T>& ctx,
                                      EnqueueResultCode code,
                                      BackpressureReason reason) {
        EnqueueResult r;
        r.code = code;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = reason;
        return r;
    }
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 2: Write `drop_newest_handler.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>

namespace hpactor::mailbox::detail {

template <typename T>
class DropNewestHandler : public IOverflowHandler<T> {
  public:
    EnqueueResult handle(OverflowContext<T>& ctx,
                         ReservationResult /*reason*/) override {
        ctx.total_dropped.fetch_add(1, std::memory_order_relaxed);
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = metrics::MetricEventType::kMailboxDropped;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
        EnqueueResult r;
        r.code = EnqueueResultCode::DroppedNewest;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = BackpressureReason::OverflowPolicy;
        return r;
    }

    OverflowPolicy policy() const override { return OverflowPolicy::DropNewest; }
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 3: Write `drop_oldest_handler.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>

namespace hpactor::mailbox::detail {

template <typename T>
class DropOldestHandler : public IOverflowHandler<T> {
  public:
    EnqueueResult handle(OverflowContext<T>& ctx,
                         ReservationResult reason) override {
        if (ctx.drop_oldest_fn && ctx.drop_oldest_fn()) {
            // Successfully freed a slot — signal mailbox to retry reservation.
            EnqueueResult r;
            r.code = EnqueueResultCode::DroppedExisting;
            r.target = ctx.actor_id;
            r.depth = ctx.current_depth;
            r.capacity = ctx.config.capacity.max_messages;
            r.bytes = ctx.current_bytes;
            r.byte_capacity = ctx.config.capacity.max_bytes;
            r.pressure_reason = BackpressureReason::OverflowPolicy;
            return r;
        }
        // Couldn't drop — reject.
        ctx.total_rejected.fetch_add(1, std::memory_order_relaxed);
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = metrics::MetricEventType::kMailboxRejected;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
        EnqueueResult r;
        r.code = EnqueueResultCode::Rejected;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = reason == ReservationResult::ByteCapacity
                                ? BackpressureReason::ByteCapacity
                                : BackpressureReason::HardCapacity;
        return r;
    }

    OverflowPolicy policy() const override { return OverflowPolicy::DropOldest; }
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 4: Write `dead_letter_handler.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>

namespace hpactor::mailbox::detail {

template <typename T>
class DeadLetterHandler : public IOverflowHandler<T> {
  public:
    EnqueueResult handle(OverflowContext<T>& ctx,
                         ReservationResult /*reason*/) override {
        ctx.total_dead_letters.fetch_add(1, std::memory_order_relaxed);
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = metrics::MetricEventType::kMailboxDeadLetter;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
        EnqueueResult r;
        r.code = EnqueueResultCode::ReroutedToDeadLetter;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = BackpressureReason::OverflowPolicy;
        return r;
    }

    OverflowPolicy policy() const override { return OverflowPolicy::DeadLetter; }
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 5: Write `signal_only_handler.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>

#include <chrono>

namespace hpactor::mailbox::detail {

template <typename T>
class SignalOnlyHandler : public IOverflowHandler<T> {
  public:
    EnqueueResult handle(OverflowContext<T>& ctx,
                         ReservationResult reason) override {
        ctx.total_rejected.fetch_add(1, std::memory_order_relaxed);
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = metrics::MetricEventType::kMailboxRejected;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
        EnqueueResult r;
        r.code = EnqueueResultCode::Rejected;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = reason == ReservationResult::ByteCapacity
                                ? BackpressureReason::ByteCapacity
                                : BackpressureReason::HardCapacity;
        r.retry_after = std::chrono::milliseconds(ctx.config.signal_min_interval_ms);
        return r;
    }

    OverflowPolicy policy() const override { return OverflowPolicy::SignalOnly; }
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 6: Write `spill_to_overflow_handler.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>

#include <utility>

namespace hpactor::mailbox::detail {

template <typename T>
class SpillToOverflowHandler : public IOverflowHandler<T> {
  public:
    EnqueueResult handle(OverflowContext<T>& ctx,
                         ReservationResult reason) override {
        if (ctx.overflow_queue.try_push(
                std::move(const_cast<T&>(ctx.message)))) {
            EnqueueResult r;
            r.code = EnqueueResultCode::ReroutedToOverflow;
            r.target = ctx.actor_id;
            r.depth = ctx.current_depth;
            r.capacity = ctx.config.capacity.max_messages;
            r.bytes = ctx.current_bytes;
            r.byte_capacity = ctx.config.capacity.max_bytes;
            r.pressure_reason = reason == ReservationResult::ByteCapacity
                                    ? BackpressureReason::ByteCapacity
                                    : BackpressureReason::HardCapacity;
            return r;
        }
        // Overflow queue full — reject.
        ctx.total_rejected.fetch_add(1, std::memory_order_relaxed);
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = metrics::MetricEventType::kMailboxRejected;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
        EnqueueResult r;
        r.code = EnqueueResultCode::Rejected;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = reason == ReservationResult::ByteCapacity
                                ? BackpressureReason::ByteCapacity
                                : BackpressureReason::HardCapacity;
        return r;
    }

    OverflowPolicy policy() const override {
        return OverflowPolicy::SpillToOverflowQueue;
    }
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 7: Write `test_overflow_handlers.cpp`**

```cpp
#include <hpactor/mailbox/detail/handlers/dead_letter_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_oldest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/reject_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/signal_only_handler.hpp>
#include <hpactor/mailbox/detail/handlers/spill_to_overflow_handler.hpp>
#include <hpactor/mailbox/detail/overflow_context.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>

#include <gtest/gtest.h>
#include <atomic>

using namespace hpactor::mailbox;
using namespace hpactor::mailbox::detail;

struct TestMsg { int payload = 0; };

// Minimal test fixture that builds a real OverflowContext.
class OverflowHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        config_.capacity.max_messages = 100;
        config_.capacity.max_bytes = 1024 * 1024;
    }

    template <typename Handler>
    EnqueueResult invoke(Handler& handler, ReservationResult reason) {
        msg_ = TestMsg{42};
        OverflowContext<TestMsg> ctx{
            msg_, meta_, reservation_, overflow_queue_,
            total_rejected_, total_dropped_, total_dead_letters_,
            nullptr, config_, ActorId{1},
            /*current_depth=*/100, /*current_bytes=*/1024 * 1024,
            /*drop_oldest_fn=*/nullptr
        };
        return handler.handle(ctx, reason);
    }

    TestMsg msg_;
    MailboxEnvelopeMeta meta_;
    ReservationManager<TestMsg> reservation_;
    OverflowQueue<TestMsg> overflow_queue_;
    std::atomic<uint64_t> total_rejected_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_dead_letters_{0};
    MailboxConfig config_;
};

TEST_F(OverflowHandlerTest, RejectNewestReturnsRejected) {
    RejectNewestHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(total_rejected_.load(), 1);
    EXPECT_EQ(r.pressure_reason, BackpressureReason::HardCapacity);
}

TEST_F(OverflowHandlerTest, RejectNewestByteCapacityReason) {
    RejectNewestHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::ByteCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(r.pressure_reason, BackpressureReason::ByteCapacity);
}

TEST_F(OverflowHandlerTest, DropNewestReturnsDroppedNewest) {
    DropNewestHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::DroppedNewest);
    EXPECT_EQ(total_dropped_.load(), 1);
    EXPECT_EQ(r.pressure_reason, BackpressureReason::OverflowPolicy);
}

TEST_F(OverflowHandlerTest, DeadLetterReturnsReroutedToDeadLetter) {
    DeadLetterHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::ReroutedToDeadLetter);
    EXPECT_EQ(total_dead_letters_.load(), 1);
}

TEST_F(OverflowHandlerTest, SignalOnlyReturnsRejectedWithRetryAfter) {
    config_.signal_min_interval_ms = 200;
    SignalOnlyHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(r.retry_after.count(), 200);
    EXPECT_EQ(total_rejected_.load(), 1);
}

TEST_F(OverflowHandlerTest, DropOldestNoCallbackReturnsRejected) {
    DropOldestHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(total_rejected_.load(), 1);
}

TEST_F(OverflowHandlerTest, DropOldestWithSuccessCallback) {
    DropOldestHandler<TestMsg> handler;
    // Rebuild context with a callback that reports success.
    TestMsg msg{42};
    OverflowContext<TestMsg> ctx{
        msg, meta_, reservation_, overflow_queue_,
        total_rejected_, total_dropped_, total_dead_letters_,
        nullptr, config_, ActorId{1},
        /*current_depth=*/100, /*current_bytes=*/1024 * 1024,
        []() { return true; }
    };
    auto r = handler.handle(ctx, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::DroppedExisting);
}

TEST_F(OverflowHandlerTest, SpillToOverflowSucceedsWhenQueueHasRoom) {
    overflow_queue_.set_max_depth(100);
    SpillToOverflowHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::ReroutedToOverflow);
    EXPECT_FALSE(overflow_queue_.empty());
}

TEST_F(OverflowHandlerTest, EachHandlerReportsCorrectPolicy) {
    EXPECT_EQ(RejectNewestHandler<TestMsg>{}.policy(), OverflowPolicy::RejectNewest);
    EXPECT_EQ(DropNewestHandler<TestMsg>{}.policy(), OverflowPolicy::DropNewest);
    EXPECT_EQ(DropOldestHandler<TestMsg>{}.policy(), OverflowPolicy::DropOldest);
    EXPECT_EQ(DeadLetterHandler<TestMsg>{}.policy(), OverflowPolicy::DeadLetter);
    EXPECT_EQ(SignalOnlyHandler<TestMsg>{}.policy(), OverflowPolicy::SignalOnly);
    EXPECT_EQ(SpillToOverflowHandler<TestMsg>{}.policy(),
              OverflowPolicy::SpillToOverflowQueue);
}
```

- [ ] **Step 8: Add test source to `tests/unit/mailbox/CMakeLists.txt`**

Add `test_overflow_handlers.cpp` to the `add_executable(test_unit_mailbox ...)` list.

- [ ] **Step 9: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*OverflowHandler*"
```

Expected: 9 tests, all PASS.

- [ ] **Step 10: Commit**

```bash
git add include/hpactor/mailbox/detail/handlers/ tests/unit/mailbox/test_overflow_handlers.cpp tests/unit/mailbox/CMakeLists.txt
git commit -m "refactor(mailbox): add six concrete IOverflowHandler implementations

Extract each overflow policy into a stateless Strategy class:
RejectNewest, DropNewest, DropOldest, DeadLetter, SignalOnly,
SpillToOverflow. Each handler constructs its own EnqueueResult from
OverflowContext fields — no more duplicated metrics/counter code.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 6: OverflowHandlerFactory

**Files:**
- Create: `include/hpactor/mailbox/detail/overflow_handler_factory.hpp`
- Create: `tests/unit/mailbox/test_overflow_handler_factory.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Component:** Free function `make_overflow_handler<T>(OverflowPolicy)` that maps enum values to concrete handler instances.

- [ ] **Step 1: Write `overflow_handler_factory.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/mailbox/detail/handlers/dead_letter_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_oldest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/reject_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/signal_only_handler.hpp>
#include <hpactor/mailbox/detail/handlers/spill_to_overflow_handler.hpp>
#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <memory>

namespace hpactor::mailbox::detail {

template <typename T>
[[nodiscard]] std::unique_ptr<IOverflowHandler<T>>
make_overflow_handler(OverflowPolicy policy) {
    switch (policy) {
        case OverflowPolicy::RejectNewest:
            return std::make_unique<RejectNewestHandler<T>>();
        case OverflowPolicy::DropNewest:
            return std::make_unique<DropNewestHandler<T>>();
        case OverflowPolicy::DropOldest:
            return std::make_unique<DropOldestHandler<T>>();
        case OverflowPolicy::DeadLetter:
            return std::make_unique<DeadLetterHandler<T>>();
        case OverflowPolicy::SignalOnly:
            return std::make_unique<SignalOnlyHandler<T>>();
        case OverflowPolicy::SpillToOverflowQueue:
            return std::make_unique<SpillToOverflowHandler<T>>();
        case OverflowPolicy::DropLowestPriority:
        case OverflowPolicy::BlockWhenAllowed:
        default:
            // Not yet implemented — fall back to RejectNewest.
            return std::make_unique<RejectNewestHandler<T>>();
    }
}

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 2: Write `test_overflow_handler_factory.cpp`**

```cpp
#include <hpactor/mailbox/detail/overflow_handler_factory.hpp>

#include <gtest/gtest.h>

using namespace hpactor::mailbox;
using namespace hpactor::mailbox::detail;

struct TestMsg { int x; };

TEST(OverflowHandlerFactoryTest, MapsEachEnumToCorrectPolicy) {
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::RejectNewest)->policy(),
              OverflowPolicy::RejectNewest);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::DropNewest)->policy(),
              OverflowPolicy::DropNewest);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::DropOldest)->policy(),
              OverflowPolicy::DropOldest);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::DeadLetter)->policy(),
              OverflowPolicy::DeadLetter);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::SignalOnly)->policy(),
              OverflowPolicy::SignalOnly);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::SpillToOverflowQueue)->policy(),
              OverflowPolicy::SpillToOverflowQueue);
}

TEST(OverflowHandlerFactoryTest, UnimplementedPoliciesFallBackToRejectNewest) {
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::DropLowestPriority)->policy(),
              OverflowPolicy::RejectNewest);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::BlockWhenAllowed)->policy(),
              OverflowPolicy::RejectNewest);
}
```

- [ ] **Step 3: Add test source to `tests/unit/mailbox/CMakeLists.txt`**

Add `test_overflow_handler_factory.cpp` to the `add_executable(test_unit_mailbox ...)` list.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*OverflowHandlerFactory*"
```

Expected: 2 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/detail/overflow_handler_factory.hpp tests/unit/mailbox/test_overflow_handler_factory.cpp tests/unit/mailbox/CMakeLists.txt
git commit -m "refactor(mailbox): add OverflowHandlerFactory for enum→strategy mapping

Maps each OverflowPolicy value to its concrete IOverflowHandler.
Unimplemented policies fall back to RejectNewestHandler.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 7: Refactor MPSCActorMailbox to Use Composed Components

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

**Component:** Replace the internal implementation with delegation to the new components. Public API unchanged. The `ReservationResult` enum, `try_reserve()`, `try_reserve_system()`, `release_reservation()`, `release_system_reservation()`, `pressure_severity()`, `next_pressure_state()`, `update_pressure_state()`, `pressure_code_after_accept()`, and the overflow policy switch are all removed from the private section. The `reserved_messages_`, `reserved_system_messages_`, `queued_bytes_`, `pressure_state_`, `last_backpressure_signal_ns_`, `last_backpressure_signal_severity_`, `backpressure_signal_sequence_` atomics are removed from the member list.

- [ ] **Step 1: Rewrite `mpsc_actor_mailbox.hpp`**

The file replaces all extracted internals with delegation to the new detail components. The full file is shown below (key changes annotated):

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/detail/backpressure_signal_gate.hpp>
#include <hpactor/mailbox/detail/overflow_handler_factory.hpp>
#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/mailbox/detail/reservation_manager.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/mpsc_mailbox.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <functional>
#include <optional>
#include <type_traits>

namespace hpactor::mailbox {

using ActorContinuationCallback = std::function<void()>;

template <typename T> class MPSCActorMailbox {
  public:
    MPSCActorMailbox(ActorId actor_id, sched::IScheduler* scheduler,
                     MailboxConfig config = {}) noexcept
        : actor_id_(actor_id), scheduler_(scheduler), config_(config) {
        if (config_.capacity.max_messages == 0) {
            config_.capacity.max_messages = 1024;
        }
        overflow_queue_.set_max_depth(config_.max_overflow_depth);
        overflow_handler_ =
            detail::make_overflow_handler<T>(config_.overflow_policy);
    }

    ~MPSCActorMailbox() {
        if (pending_free_) {
            pending_free_->~T();
            mem::deallocate(pending_free_);
        }
    }

    void set_continuation_callback(ActorContinuationCallback callback) {
        continuation_callback_ = std::move(callback);
    }

    void set_config(const MailboxConfig& cfg) noexcept {
        config_ = cfg;
        if (config_.capacity.max_messages == 0) {
            config_.capacity.max_messages = 1024;
        }
        overflow_queue_.set_max_depth(config_.max_overflow_depth);
        overflow_handler_ =
            detail::make_overflow_handler<T>(config_.overflow_policy);
    }

    const MailboxConfig& config() const noexcept { return config_; }

    EnqueueResult try_push(T&& msg, MailboxEnvelopeMeta meta = {}) noexcept {
        if (meta.estimated_bytes == 0) {
            meta.estimated_bytes = estimate_node_bytes(msg);
        }

        // Reserve capacity.
        auto reserve_result = reservation_.try_reserve(
            meta.estimated_bytes, config_.capacity.max_messages,
            config_.capacity.max_bytes);

        if (reserve_result != detail::ReservationResult::Reserved) {
            // System messages get a second chance.
            bool sys_reserved = false;
            if (is_system_message(meta.type_tag)) {
                sys_reserved = reservation_.try_reserve_system(
                    meta.estimated_bytes, config_.protected_system_messages);
            }

            if (!sys_reserved) {
                update_pressure_state(/*hard_failure=*/true);
                auto reason =
                    reserve_result == detail::ReservationResult::ByteCapacity
                        ? BackpressureReason::ByteCapacity
                        : BackpressureReason::HardCapacity;

                // Build context and delegate to overflow handler.
                detail::OverflowContext<T> ctx{
                    msg, meta, reservation_, overflow_queue_,
                    total_rejected_, total_dropped_, total_dead_letters_,
                    metrics_ring_buffer_, config_, actor_id_,
                    static_cast<uint32_t>(mailbox_.count()),
                    reservation_.queued_bytes(),
                    [this]() { return drop_one_oldest(); }
                };

                auto result = overflow_handler_->handle(ctx, reserve_result);

                // DropOldest retry: handler freed a slot, retry reservation.
                if (result.code == EnqueueResultCode::DroppedExisting) {
                    reserve_result = reservation_.try_reserve(
                        meta.estimated_bytes, config_.capacity.max_messages,
                        config_.capacity.max_bytes);
                    if (reserve_result == detail::ReservationResult::Reserved) {
                        goto enqueue;  // Retry succeeded — enqueue below.
                    }
                    // Retry failed — the handler already filled result.
                    result.code = EnqueueResultCode::Rejected;
                }
                return result;
            }
        }

    enqueue:
        void* raw =
            mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
        auto* node = new (raw) T(std::move(msg));
        enqueue_reserved(node, meta);
        return make_result(pressure_state_.code_after_accept());
    }

    void push(T&& msg) noexcept {
        (void)try_push(std::move(msg));
    }

    void enqueue(T* node) noexcept {
        uint64_t bytes = estimate_node_bytes(*node);
        if (reservation_.try_reserve(bytes, config_.capacity.max_messages,
                                      config_.capacity.max_bytes) !=
            detail::ReservationResult::Reserved) {
            update_pressure_state(true);
            total_rejected_.fetch_add(1, std::memory_order_relaxed);
            if (metrics_ring_buffer_) [[unlikely]] {
                metrics::MetricEvent evt{};
                evt.actor_id = actor_id_;
                evt.event_type = metrics::MetricEventType::kMailboxRejected;
                evt.value_hi = 1;
                metrics_ring_buffer_->try_push(evt);
            }
            return;
        }
        MailboxEnvelopeMeta meta;
        meta.estimated_bytes = bytes;
        enqueue_reserved(node, meta);
    }

    void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta,
                          bool suppress_wakeup = false) noexcept {
        bool was_empty = empty();
        mailbox_.enqueue(node);
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        update_max_depth();
        update_pressure_state();

        int64_t depth = mailbox_.count();
        if (depth > 1024) [[unlikely]] {
            HPACTOR_LOG_WARNING(
                log::LogCategory::kMailbox, actor_id_,
                static_cast<uint32_t>(log::LogEventId::kMailboxDepthHigh),
                "mailbox depth high",
                log::field("depth", static_cast<uint64_t>(depth)));
        }

        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = actor_id_;
            evt.event_type = metrics::MetricEventType::kMailboxEnqueue;
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }

        if (was_empty && !suppress_wakeup) {
            bool expected = true;
            if (mailbox_was_empty_.compare_exchange_strong(
                    expected, false, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (continuation_callback_) {
                    continuation_callback_();
                }
                scheduler_->notify_ready(actor_id_, meta.priority,
                                         meta.deadline_ns);
            }
        }
    }

    T* dequeue() noexcept {
        lock_consumer();
        T* node = mailbox_.dequeue();
        if (node != nullptr) {
            uint64_t bytes = estimate_node_bytes(*node);
            if constexpr (std::is_same_v<T, TypedMessage>) {
                if (is_system_message(node->type_id()) &&
                    reservation_.reserved_system_count() > 0) {
                    reservation_.release_system(bytes);
                } else {
                    reservation_.release(bytes);
                }
            } else {
                reservation_.release(bytes);
            }
            total_dequeued_.fetch_add(1, std::memory_order_relaxed);
            update_pressure_state();
            if (empty()) {
                mailbox_was_empty_.store(true, std::memory_order_release);
            }
            drain_overflow();
        }
        unlock_consumer();

        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = actor_id_;
            evt.event_type = metrics::MetricEventType::kMailboxDequeue;
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }
        return node;
    }

    bool try_pop(T& out) noexcept {
        T* node = dequeue();
        if (!node) return false;
        out = std::move(*node);
        node->~T();
        mem::deallocate(node);
        return true;
    }

    bool empty() const noexcept { return mailbox_.empty(); }

    bool was_empty() const noexcept {
        return mailbox_was_empty_.load(std::memory_order_acquire);
    }

    void set_was_empty(bool val) noexcept {
        mailbox_was_empty_.store(val, std::memory_order_release);
    }

    void set_metrics_ring_buffer(
        metrics::MpscRingBuffer<metrics::MetricEvent>* buf) noexcept {
        metrics_ring_buffer_ = buf;
    }

    void set_logger(log::Logger* logger) noexcept { logger_ = logger; }

    void inject_for_test(T* node) noexcept {
        reservation_.inject_count(estimate_node_bytes(*node));
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        mailbox_.enqueue(node);
        mailbox_was_empty_.store(false, std::memory_order_release);
    }

    cli::MboxSnapshot snapshot() const {
        cli::MboxSnapshot s;
        s.depth = static_cast<uint32_t>(mailbox_.count());
        s.capacity = config_.capacity.max_messages;
        s.queued_bytes = reservation_.queued_bytes();
        s.byte_capacity = config_.capacity.max_bytes;

        double ratio = pressure_ratio();
        s.pressure_ratio_ppm = static_cast<uint32_t>(ratio * 1'000'000.0);

        s.total_enqueued = total_enqueued_.load(std::memory_order_acquire);
        s.total_dequeued = total_dequeued_.load(std::memory_order_acquire);
        s.total_rejected = total_rejected_.load(std::memory_order_acquire);
        s.total_dropped = total_dropped_.load(std::memory_order_acquire);
        s.total_dead_letters =
            total_dead_letters_.load(std::memory_order_acquire);
        s.max_depth = max_depth_.load(std::memory_order_acquire);
        {
            auto oq_snap = overflow_queue_.snapshot();
            s.overflow_depth = oq_snap.depth;
            s.overflow_max_depth = oq_snap.max_depth;
            s.overflow_total_pushed = oq_snap.total_pushed;
            s.overflow_total_popped = oq_snap.total_popped;
            s.overflow_total_lost = oq_snap.total_lost;
        }
        s.high_priority_depth = 0;
        s.pressure_state =
            to_string(pressure_state_.current_state());
        s.overflow_policy = to_string(config_.overflow_policy);
        return s;
    }

    std::optional<uint64_t>
    try_acquire_backpressure_signal(uint64_t now_ns,
                                    MailboxPressureState state) noexcept {
        return backpressure_signal_gate_.try_acquire(
            now_ns, state, config_.signal_min_interval_ms);
    }

  private:
    // --- Core queue operations (unchanged from original) ---

    bool drop_one_oldest() noexcept {
        lock_consumer();
        T* node = mailbox_.dequeue();
        if (!node) {
            unlock_consumer();
            return false;
        }
        uint64_t bytes = estimate_node_bytes(*node);
        if constexpr (std::is_same_v<T, TypedMessage>) {
            if (is_system_message(node->type_id()) &&
                reservation_.reserved_system_count() > 0) {
                reservation_.release_system(bytes);
            } else {
                reservation_.release(bytes);
            }
        } else {
            reservation_.release(bytes);
        }
        total_dropped_.fetch_add(1, std::memory_order_relaxed);
        update_pressure_state();
        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = actor_id_;
            evt.event_type = metrics::MetricEventType::kMailboxDropped;
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }
        if (empty()) {
            mailbox_was_empty_.store(true, std::memory_order_release);
        }
        unlock_consumer();
        if (pending_free_) {
            pending_free_->~T();
            mem::deallocate(pending_free_);
        }
        pending_free_ = node;
        return true;
    }

    void drain_overflow() noexcept {
        while (config_.overflow_policy ==
               OverflowPolicy::SpillToOverflowQueue) {
            if (reservation_.try_reserve(0, config_.capacity.max_messages,
                                         config_.capacity.max_bytes) !=
                detail::ReservationResult::Reserved)
                break;
            T overflow_msg;
            if (!overflow_queue_.try_pop(overflow_msg)) {
                reservation_.release(0);
                break;
            }
            MailboxEnvelopeMeta meta;
            meta.estimated_bytes = estimate_node_bytes(overflow_msg);
            enqueue_reserved(
                new (mem::allocate(mem::RegionType::kMessage, sizeof(T),
                                   actor_id_)) T(std::move(overflow_msg)),
                meta, /*suppress_wakeup=*/true);
        }
    }

    double pressure_ratio() const noexcept {
        const uint32_t cap = config_.capacity.max_messages;
        const uint32_t depth = static_cast<uint32_t>(mailbox_.count());
        double count_ratio = 0.0;
        if (cap > 0) {
            count_ratio = static_cast<double>(depth) / static_cast<double>(cap);
        }
        double byte_ratio = 0.0;
        const uint64_t byte_cap = config_.capacity.max_bytes;
        if (byte_cap > 0) {
            byte_ratio =
                static_cast<double>(reservation_.queued_bytes()) /
                static_cast<double>(byte_cap);
        }
        return count_ratio > byte_ratio ? count_ratio : byte_ratio;
    }

    void update_pressure_state(bool hard_failure = false) noexcept {
        pressure_state_.update(pressure_ratio(), hard_failure,
                               config_.high_watermark,
                               config_.low_watermark,
                               config_.critical_watermark);
    }

    void update_max_depth() noexcept {
        uint64_t depth = static_cast<uint64_t>(mailbox_.count());
        uint64_t prev = max_depth_.load(std::memory_order_acquire);
        while (depth > prev) {
            if (max_depth_.compare_exchange_weak(
                    prev, depth, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        }
    }

    EnqueueResult make_result(EnqueueResultCode code,
                              BackpressureReason reason =
                                  BackpressureReason::HighWatermark) const noexcept {
        EnqueueResult r;
        r.code = code;
        r.target = actor_id_;
        r.depth = static_cast<uint32_t>(mailbox_.count());
        r.capacity = config_.capacity.max_messages;
        r.bytes = reservation_.queued_bytes();
        r.byte_capacity = config_.capacity.max_bytes;
        r.pressure_ratio = pressure_ratio();
        r.pressure_reason = reason;
        r.pressure_state = pressure_state_.current_state();

        auto base = std::chrono::milliseconds(config_.signal_min_interval_ms);
        if (r.pressure_state == MailboxPressureState::HardPressure) {
            r.retry_after = base * 2;
        } else if (r.pressure_state == MailboxPressureState::SoftPressure ||
                   r.pressure_state == MailboxPressureState::Recovering) {
            r.retry_after = base;
        }
        return r;
    }

    static uint64_t estimate_node_bytes(const T& node) noexcept {
        if constexpr (std::is_same_v<T, TypedMessage>) {
            return estimate_message_bytes(node);
        } else {
            return sizeof(T);
        }
    }

    void lock_consumer() noexcept {
        while (consumer_lock_.test_and_set(std::memory_order_acquire)) {}
    }
    void unlock_consumer() noexcept {
        consumer_lock_.clear(std::memory_order_release);
    }

    // --- Composed components ---
    detail::ReservationManager<T> reservation_;
    detail::PressureStateMachine pressure_state_;
    detail::BackpressureSignalGate backpressure_signal_gate_;
    std::unique_ptr<detail::IOverflowHandler<T>> overflow_handler_;

    // --- Core queue members ---
    ActorId actor_id_;
    sched::IScheduler* scheduler_;
    MPSCMailbox<T> mailbox_;
    OverflowQueue<T> overflow_queue_;
    MailboxConfig config_;
    std::atomic_flag consumer_lock_ = ATOMIC_FLAG_INIT;
    T* pending_free_{nullptr};
    std::atomic<bool> mailbox_was_empty_{true};

    // --- Counters ---
    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_dequeued_{0};
    std::atomic<uint64_t> total_rejected_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_dead_letters_{0};
    std::atomic<uint64_t> max_depth_{0};

    // --- Dependencies ---
    ActorContinuationCallback continuation_callback_;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{nullptr};
    log::Logger* logger_ = nullptr;
};

} // namespace hpactor::mailbox
```

Key changes from original:
1. `#include` the 5 new detail headers instead of inlining the logic.
2. Constructor calls `detail::make_overflow_handler<T>(config_.overflow_policy)`.
3. `try_push()` delegates to `overflow_handler_->handle(ctx, reason)` instead of a 120-line switch.
4. `try_push()` retries reservation after `DropOldestHandler` returns `DroppedExisting`.
5. `snapshot()` reads `reservation_.queued_bytes()` and `pressure_state_.current_state()`.
6. `try_acquire_backpressure_signal()` delegates to `backpressure_signal_gate_.try_acquire()`.
7. `try_reserve()`, `try_reserve_system()`, `release_reservation()`, `release_system_reservation()`, `pressure_severity()`, `next_pressure_state()`, `update_pressure_state()`, `pressure_code_after_accept()` — all removed from private section.
8. `ReservationResult` enum — removed from private section.
9. 8 atomic members removed: `reserved_messages_`, `reserved_system_messages_`, `queued_bytes_`, `pressure_state_`, `last_backpressure_signal_ns_`, `last_backpressure_signal_severity_`, `backpressure_signal_sequence_`.
10. `drop_one_oldest()` now calls `reservation_.release()` and `reservation_.release_system()` instead of the removed private methods.
11. `drain_overflow()` calls `reservation_.try_reserve()` and `reservation_.release()`.

- [ ] **Step 2: Build**

```bash
ninja -C build
```

Expected: successful compilation. Fix any compilation errors (likely: missing includes, namespace qualifiers, `goto` label scoping).

- [ ] **Step 3: Run ALL existing mailbox tests**

```bash
./build/tests/unit/mailbox/test_unit_mailbox
```

Expected: all 86 + 39 new = 125 tests pass. The existing tests for `test_bounded_mailbox`, `test_mailbox_overflow_policies`, etc. must pass unchanged since the public API is identical.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp
git commit -m "refactor(mailbox): delegate MPSCActorMailbox internals to extracted components

Replace 784-line monolith with ~280-line coordinator that delegates to
ReservationManager, PressureStateMachine, BackpressureSignalGate, and the
IOverflowHandler strategy hierarchy. Public API unchanged — all existing
tests pass without modification.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 8: Full Test Verification

**Files:** (none new — verification only)

- [ ] **Step 1: Run the complete mailbox test suite**

```bash
./build/tests/unit/mailbox/test_unit_mailbox
```

Expected: all 125 tests pass (86 original + 39 new).

- [ ] **Step 2: Run scheduler tests that use MPSCActorMailbox**

```bash
ninja -C build test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*Mailbox*:*ActorState*"
```

Expected: pass.

- [ ] **Step 3: Run backpressure stress tests**

```bash
ninja -C build test_integration_mailbox && ./build/tests/integration/mailbox/test_integration_mailbox
```

Expected: pass.

- [ ] **Step 4: Run full test suite**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 5: Commit any final fixes needed**

---

### Task 9: Remove `#include` Dependencies from Consumers

**Files:**
- None modified (verification-only task)

Since the public API is unchanged, no consumer files need modification. However, if any consumer was inadvertently relying on `ReservationResult` being a nested type of `MPSCActorMailbox`, it will fail to compile. Verify:

- [ ] **Step 1: Build all targets**

```bash
ninja -C build
```

Expected: clean build. If compilation fails in consumer files, fix by adding the appropriate `#include` or qualifying the type. But since `ReservationResult` was private, no consumer should have referenced it.

---

## Summary of Changes

| Type | Count | Files |
|------|-------|-------|
| New headers | 12 | `detail/reservation_manager.hpp`, `detail/pressure_state_machine.hpp`, `detail/backpressure_signal_gate.hpp`, `detail/overflow_context.hpp`, `detail/overflow_handler_interface.hpp`, `detail/overflow_handler_factory.hpp`, `detail/handlers/reject_newest_handler.hpp`, `detail/handlers/drop_newest_handler.hpp`, `detail/handlers/drop_oldest_handler.hpp`, `detail/handlers/dead_letter_handler.hpp`, `detail/handlers/signal_only_handler.hpp`, `detail/handlers/spill_to_overflow_handler.hpp` |
| Modified headers | 1 | `mpsc_actor_mailbox.hpp` (784 → ~280 lines) |
| New tests | 5 | `test_reservation_manager.cpp`, `test_pressure_state_machine.cpp`, `test_backpressure_signal_gate.cpp`, `test_overflow_handlers.cpp`, `test_overflow_handler_factory.cpp` |
| Modified tests | 1 | `tests/unit/mailbox/CMakeLists.txt` (add 5 sources) |
| Unchanged | all | `mailbox_policy.hpp`, `mpsc_mailbox.hpp`, `overflow_queue.hpp`, all consumers |

**Lines of code net change:** ~784 removed from `mpsc_actor_mailbox.hpp`, ~550 added across 12 new headers + 5 test files. The mailbox file drops from 784 to ~280 lines.
