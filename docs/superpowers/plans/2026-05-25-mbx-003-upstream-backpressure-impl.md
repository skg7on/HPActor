# MBX-003 Upstream Backpressure Signal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement local and remote upstream backpressure signals for mailbox pressure, including low/high/critical state, retry-after metadata, rate limiting, metrics, logs, CLI exposure, and focused tests.

**Architecture:** Extend the existing bounded mailbox admission path rather than adding a new queue. `MPSCActorMailbox` owns pressure-state calculation and signal-budget rate limiting; `ActorSystem::try_deliver_local()` owns signal emission; remote propagation uses the existing `ActorMsgFrame` plus `TypeTag::BackpressureSignalTag` and is intercepted by `ActorSystem::deliver_remote()`.

**Tech Stack:** C++20, existing HPActor mailbox/config/transport/protobuf/metrics/CLI subsystems, Google Test, CMake protobuf codegen.

---

## File Structure

- Modify `include/hpactor/mailbox/mailbox_policy.hpp`: add `critical_watermark`, result pressure fields, and string helpers.
- Modify `include/hpactor/config/mailbox_fields.def`: add `critical_watermark` default config field.
- Modify `src/config/parsers/mailbox_config_parser.cpp`: parse and normalize critical watermark.
- Modify `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`: compute count/byte pressure, drive hysteresis, expose signal budget, and populate richer `EnqueueResult`.
- Modify `protos/hpactor/messages.proto`: add `BackpressureSignalMessage`.
- Modify `include/hpactor/core/proto_type_registry.hpp` and `src/core/proto_type_registry.cpp`: register message traits and system type.
- Create `include/hpactor/mailbox/backpressure_signal_serialization.hpp`: declare signal protobuf conversion helpers.
- Create `src/mailbox/backpressure_signal_serialization.cpp`: implement signal protobuf conversion helpers.
- Modify `src/CMakeLists.txt`: compile the new mailbox serialization source.
- Modify `include/hpactor/core/actor_system.hpp`: declare helper methods for signal emission and remote interception.
- Modify `src/actor/actor_system.cpp`: emit local/remote signals, intercept remote signal frames, and record metrics/logs.
- Modify `include/hpactor/cli/cli_types.hpp`, `protos/hpactor/cli_messages.proto`, `src/actor/event_based_actor.cpp`, and `src/cli/commands/actor_commands.cpp`: expose pressure data in CLI inspection.
- Add/modify tests in `tests/unit/mailbox/test_bounded_mailbox.cpp`.
- Modify `tests/integration/actor/test_backpressure_signals.cpp`.
- Modify `tests/integration/actor/test_actor_system_backpressure.cpp`.
- Create `tests/integration/actor/test_remote_backpressure_signals.cpp`: cover inbound remote signal interception and outbound control frame construction through a test wire sink.
- Modify `tests/integration/metrics/test_metrics_aggregator.cpp`: cover backpressure signal counter events with reason/state metadata.
- Modify `tests/integration/actor/test_actor_system_backpressure.cpp`: cover the mailbox snapshot fields consumed by CLI actor inspection.

---

### Task 1: Extend Mailbox Policy Types

**Files:**
- Modify: `include/hpactor/mailbox/mailbox_policy.hpp`
- Modify: `include/hpactor/config/mailbox_fields.def`
- Modify: `src/config/parsers/mailbox_config_parser.cpp`
- Test: `tests/unit/mailbox/test_bounded_mailbox.cpp`

- [ ] **Step 1: Add failing config/type expectations**

Append to `tests/unit/mailbox/test_bounded_mailbox.cpp`:

```cpp
TEST(MailboxPolicyTest, DefaultCriticalWatermarkIsCapacity) {
    hpactor::mailbox::MailboxConfig cfg;
    EXPECT_DOUBLE_EQ(cfg.critical_watermark, 1.0);
}

TEST(MailboxPolicyTest, PressureStateToStringCoversAllStates) {
    using hpactor::mailbox::MailboxPressureState;
    EXPECT_STREQ(hpactor::mailbox::to_string(MailboxPressureState::Normal),
                 "normal");
    EXPECT_STREQ(hpactor::mailbox::to_string(MailboxPressureState::SoftPressure),
                 "soft_pressure");
    EXPECT_STREQ(hpactor::mailbox::to_string(MailboxPressureState::HardPressure),
                 "hard_pressure");
    EXPECT_STREQ(hpactor::mailbox::to_string(MailboxPressureState::Recovering),
                 "recovering");
}
```

- [ ] **Step 2: Run the focused mailbox tests and verify failure**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="MailboxPolicyTest.*"
```

Expected: fail to compile because `MailboxConfig::critical_watermark` does not exist.

- [ ] **Step 3: Add `critical_watermark` to runtime config**

In `include/hpactor/mailbox/mailbox_policy.hpp`, add the field after `low_watermark`:

```cpp
double high_watermark = 0.80;
double low_watermark = 0.50;
double critical_watermark = 1.00;
```

In `include/hpactor/config/mailbox_fields.def`, add:

```cpp
HPACTOR_MAILBOX_FIELD(critical_watermark,     double,                         "critical_watermark",      1.00)
```

Place it immediately after `low_watermark`.

In `src/config/parsers/mailbox_config_parser.cpp`, parse and normalize:

```cpp
out.mailbox.high_watermark = mt.read_double("high_watermark", 0.80);
out.mailbox.low_watermark = mt.read_double("low_watermark", 0.50);
out.mailbox.critical_watermark =
    mt.read_double("critical_watermark", 1.00);

if (out.mailbox.low_watermark < 0.0) {
    out.mailbox.low_watermark = 0.50;
}
if (out.mailbox.high_watermark < out.mailbox.low_watermark) {
    out.mailbox.high_watermark = 0.80;
}
if (out.mailbox.critical_watermark < out.mailbox.high_watermark ||
    out.mailbox.critical_watermark > 1.0) {
    out.mailbox.critical_watermark = 1.00;
}
```

- [ ] **Step 4: Extend `EnqueueResult` pressure fields**

In `include/hpactor/mailbox/mailbox_policy.hpp`, extend `EnqueueResult`:

```cpp
uint64_t bytes = 0;
uint64_t byte_capacity = 0;
mailbox::BackpressureReason pressure_reason =
    mailbox::BackpressureReason::HighWatermark;
mailbox::MailboxPressureState pressure_state =
    mailbox::MailboxPressureState::Normal;
```

Add these fields after `capacity` and before `pressure_ratio`, adjusting the namespace if the compiler requires unqualified names inside `hpactor::mailbox`.

- [ ] **Step 5: Run tests**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="MailboxPolicyTest.*"
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mailbox/mailbox_policy.hpp include/hpactor/config/mailbox_fields.def src/config/parsers/mailbox_config_parser.cpp tests/unit/mailbox/test_bounded_mailbox.cpp
git commit -m "feat(mailbox): add critical pressure watermark"
```

---

### Task 2: Implement Pressure State Hysteresis

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Test: `tests/unit/mailbox/test_bounded_mailbox.cpp`

- [ ] **Step 1: Add failing pressure-state tests**

Append to `tests/unit/mailbox/test_bounded_mailbox.cpp`:

```cpp
TEST_F(BoundedMailboxTest, PressureStateUsesLowHighCriticalHysteresis) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    cfg.capacity.max_messages = 4;
    cfg.high_watermark = 0.50;
    cfg.low_watermark = 0.25;
    cfg.critical_watermark = 1.00;

    MPSCActorMailbox<TypedMessage> mb(ActorId{88}, &scheduler, cfg);
    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    EXPECT_EQ(mb.snapshot().pressure_state, "normal");

    EXPECT_TRUE(mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta)
                    .accepted());
    EXPECT_EQ(mb.snapshot().pressure_state, "normal");

    auto soft =
        mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_TRUE(soft.accepted());
    EXPECT_EQ(soft.pressure_state, MailboxPressureState::SoftPressure);
    EXPECT_EQ(mb.snapshot().pressure_state, "soft_pressure");

    EXPECT_TRUE(mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{3}), meta)
                    .accepted());
    auto hard =
        mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{4}), meta);
    EXPECT_TRUE(hard.accepted());
    EXPECT_EQ(hard.pressure_state, MailboxPressureState::HardPressure);
    EXPECT_EQ(mb.snapshot().pressure_state, "hard_pressure");

    TypedMessage out;
    EXPECT_TRUE(mb.try_pop(out));
    EXPECT_EQ(mb.snapshot().pressure_state, "recovering");

    EXPECT_TRUE(mb.try_pop(out));
    EXPECT_TRUE(mb.try_pop(out));
    EXPECT_EQ(mb.snapshot().pressure_state, "recovering");

    EXPECT_TRUE(mb.try_pop(out));
    EXPECT_EQ(mb.snapshot().pressure_state, "normal");
}

TEST_F(ByteBudgetTest, BytePressureDrivesPressureState) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    cfg.capacity.max_messages = 100;
    cfg.high_watermark = 0.50;
    cfg.low_watermark = 0.25;
    cfg.critical_watermark = 1.00;

    const uint64_t base =
        estimate_message_bytes(TypedMessage(TypeTag::User, StreamBuffer{}));
    cfg.capacity.max_bytes = (base + 10) * 2;

    MPSCActorMailbox<TypedMessage> mb(ActorId{89}, &scheduler, cfg);
    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = mb.try_push(TypedMessage(TypeTag::User,
                                       StreamBuffer{1, 2, 3, 4, 5, 6, 7, 8, 9,
                                                    10}),
                          meta);
    EXPECT_TRUE(r1.accepted());
    EXPECT_EQ(r1.pressure_state, MailboxPressureState::SoftPressure);
    EXPECT_GE(r1.pressure_ratio, 0.50);
}
```

- [ ] **Step 2: Run tests and verify failure**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="BoundedMailboxTest.PressureStateUsesLowHighCriticalHysteresis:ByteBudgetTest.BytePressureDrivesPressureState"
```

Expected: fail because only `Normal` and `SoftPressure` are currently stored and byte pressure is not part of pressure ratio.

- [ ] **Step 3: Add pressure helper functions**

In `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`, add private helpers before `pressure_code_after_accept()`:

```cpp
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
        byte_ratio = static_cast<double>(
                         queued_bytes_.load(std::memory_order_acquire)) /
                     static_cast<double>(byte_cap);
    }

    return count_ratio > byte_ratio ? count_ratio : byte_ratio;
}

MailboxPressureState next_pressure_state(double ratio,
                                         bool hard_failure) const noexcept {
    if (hard_failure || ratio >= config_.critical_watermark) {
        return MailboxPressureState::HardPressure;
    }

    auto current = pressure_state_.load(std::memory_order_acquire);
    if (current == MailboxPressureState::HardPressure ||
        current == MailboxPressureState::Recovering) {
        if (ratio <= config_.low_watermark) {
            return MailboxPressureState::Normal;
        }
        return MailboxPressureState::Recovering;
    }

    if (ratio >= config_.high_watermark) {
        return MailboxPressureState::SoftPressure;
    }
    if (ratio <= config_.low_watermark) {
        return MailboxPressureState::Normal;
    }
    return current;
}
```

- [ ] **Step 4: Replace pressure update logic**

Replace `update_pressure_state()` with:

```cpp
void update_pressure_state(bool hard_failure = false) noexcept {
    const double ratio = pressure_ratio();
    pressure_state_.store(next_pressure_state(ratio, hard_failure),
                          std::memory_order_release);
}
```

In `dequeue()` and `drop_one_oldest()`, call `update_pressure_state()` after releasing reservations and before checking `empty()`.

- [ ] **Step 5: Populate richer result state**

Replace `pressure_code_after_accept()` with:

```cpp
EnqueueResultCode pressure_code_after_accept() const noexcept {
    auto state = pressure_state_.load(std::memory_order_acquire);
    if (state == MailboxPressureState::SoftPressure ||
        state == MailboxPressureState::HardPressure ||
        state == MailboxPressureState::Recovering) {
        return EnqueueResultCode::AcceptedWithSoftPressure;
    }
    return EnqueueResultCode::Accepted;
}
```

Replace `make_result()` with a version that fills bytes, byte capacity, ratio, state, and retry-after:

```cpp
EnqueueResult make_result(EnqueueResultCode code,
                          BackpressureReason reason =
                              BackpressureReason::HighWatermark) const noexcept {
    EnqueueResult r;
    r.code = code;
    r.target = actor_id_;
    r.depth = static_cast<uint32_t>(mailbox_.count());
    r.capacity = config_.capacity.max_messages;
    r.bytes = queued_bytes_.load(std::memory_order_acquire);
    r.byte_capacity = config_.capacity.max_bytes;
    r.pressure_ratio = pressure_ratio();
    r.pressure_reason = reason;
    r.pressure_state = pressure_state_.load(std::memory_order_acquire);

    auto base = std::chrono::milliseconds(config_.signal_min_interval_ms);
    if (r.pressure_state == MailboxPressureState::HardPressure) {
        r.retry_after = base * 2;
    } else if (r.pressure_state == MailboxPressureState::SoftPressure ||
               r.pressure_state == MailboxPressureState::Recovering) {
        r.retry_after = base;
    }
    return r;
}
```

When a reservation fails in `try_push()`, call `update_pressure_state(true)` before applying the overflow policy. Use `BackpressureReason::HardCapacity` for count-capacity failures and `BackpressureReason::ByteCapacity` for byte-capacity failures after Task 3 distinguishes them.

- [ ] **Step 6: Run tests**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="BoundedMailboxTest.*:ByteBudgetTest.*"
```

Expected: pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp tests/unit/mailbox/test_bounded_mailbox.cpp
git commit -m "feat(mailbox): track pressure state hysteresis"
```

---

### Task 3: Distinguish Count and Byte Capacity Failures

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Test: `tests/unit/mailbox/test_bounded_mailbox.cpp`

- [ ] **Step 1: Add failing reason tests**

Append to `tests/unit/mailbox/test_bounded_mailbox.cpp`:

```cpp
TEST_F(BoundedMailboxTest, CountCapacityFailureReportsHardCapacity) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    cfg.capacity.max_messages = 1;
    MPSCActorMailbox<TypedMessage> mb(ActorId{90}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    ASSERT_TRUE(mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta)
                    .accepted());
    auto rejected =
        mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_EQ(rejected.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(rejected.pressure_reason, BackpressureReason::HardCapacity);
    EXPECT_EQ(rejected.pressure_state, MailboxPressureState::HardPressure);
}

TEST_F(ByteBudgetTest, ByteCapacityFailureReportsByteCapacity) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    const uint64_t base =
        estimate_message_bytes(TypedMessage(TypeTag::User, StreamBuffer{}));
    cfg.capacity.max_messages = 100;
    cfg.capacity.max_bytes = base + 1;

    MPSCActorMailbox<TypedMessage> mb(ActorId{91}, &scheduler, cfg);
    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto rejected =
        mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1, 2}), meta);
    EXPECT_EQ(rejected.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(rejected.pressure_reason, BackpressureReason::ByteCapacity);
    EXPECT_EQ(rejected.pressure_state, MailboxPressureState::HardPressure);
}
```

- [ ] **Step 2: Run tests and verify failure**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="BoundedMailboxTest.CountCapacityFailureReportsHardCapacity:ByteBudgetTest.ByteCapacityFailureReportsByteCapacity"
```

Expected: fail because reservation only returns a boolean.

- [ ] **Step 3: Add reservation result enum**

Inside the private section of `MPSCActorMailbox`, before `try_reserve()`, add:

```cpp
enum class ReservationResult : uint8_t {
    Reserved,
    CountCapacity,
    ByteCapacity,
};
```

Change `try_reserve(uint64_t bytes)` to return `ReservationResult`. Return:

```cpp
return ReservationResult::CountCapacity;
```

when message count is exhausted,

```cpp
return ReservationResult::ByteCapacity;
```

when byte budget is exhausted, and

```cpp
return ReservationResult::Reserved;
```

when the reservation succeeds.

- [ ] **Step 4: Update call sites**

In `try_push()`, replace:

```cpp
if (!try_reserve(meta.estimated_bytes)) {
```

with:

```cpp
auto reserve_result = try_reserve(meta.estimated_bytes);
if (reserve_result != ReservationResult::Reserved) {
    update_pressure_state(true);
    auto reserve_reason =
        reserve_result == ReservationResult::ByteCapacity
            ? BackpressureReason::ByteCapacity
            : BackpressureReason::HardCapacity;
```

Use `reserve_reason` in `make_result()` calls for rejection paths. For overflow-policy-specific paths, use `BackpressureReason::OverflowPolicy`.

In `enqueue(T* node)`, treat any non-reserved result as rejection:

```cpp
auto reserve_result = try_reserve(bytes);
if (reserve_result != ReservationResult::Reserved) {
    update_pressure_state(true);
    total_rejected_.fetch_add(1, std::memory_order_relaxed);
    ...
    return;
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="BoundedMailboxTest.*:ByteBudgetTest.*"
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp tests/unit/mailbox/test_bounded_mailbox.cpp
git commit -m "feat(mailbox): report pressure admission reasons"
```

---

### Task 4: Add Signal Budget Rate Limiting

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Test: `tests/unit/mailbox/test_bounded_mailbox.cpp`

- [ ] **Step 1: Add failing deterministic budget tests**

Append to `tests/unit/mailbox/test_bounded_mailbox.cpp`:

```cpp
TEST_F(BoundedMailboxTest, BackpressureSignalBudgetRateLimitsSameSeverity) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    cfg.signal_min_interval_ms = 100;
    MPSCActorMailbox<TypedMessage> mb(ActorId{92}, &scheduler, cfg);

    auto first = mb.try_acquire_backpressure_signal(
        1'000'000'000ULL, MailboxPressureState::SoftPressure);
    ASSERT_TRUE(first.has_value());

    auto second = mb.try_acquire_backpressure_signal(
        1'050'000'000ULL, MailboxPressureState::SoftPressure);
    EXPECT_FALSE(second.has_value());

    auto third = mb.try_acquire_backpressure_signal(
        1'101'000'000ULL, MailboxPressureState::SoftPressure);
    EXPECT_TRUE(third.has_value());
    EXPECT_GT(third.value(), first.value());
}

TEST_F(BoundedMailboxTest, BackpressureSignalBudgetAllowsEscalation) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    cfg.signal_min_interval_ms = 100;
    MPSCActorMailbox<TypedMessage> mb(ActorId{93}, &scheduler, cfg);

    auto soft = mb.try_acquire_backpressure_signal(
        2'000'000'000ULL, MailboxPressureState::SoftPressure);
    ASSERT_TRUE(soft.has_value());

    auto hard = mb.try_acquire_backpressure_signal(
        2'010'000'000ULL, MailboxPressureState::HardPressure);
    EXPECT_TRUE(hard.has_value());
    EXPECT_GT(hard.value(), soft.value());
}
```

- [ ] **Step 2: Run tests and verify failure**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="BoundedMailboxTest.BackpressureSignalBudget*"
```

Expected: fail to compile because `try_acquire_backpressure_signal()` does not exist.

- [ ] **Step 3: Add public signal-budget method**

In `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`, add `#include <optional>`.

Add a public method near `snapshot()`:

```cpp
std::optional<uint64_t>
try_acquire_backpressure_signal(uint64_t now_ns,
                                MailboxPressureState state) noexcept {
    const uint64_t interval_ns =
        static_cast<uint64_t>(config_.signal_min_interval_ms) * 1'000'000ULL;
    const auto severity = pressure_severity(state);

    uint64_t last = last_backpressure_signal_ns_.load(std::memory_order_acquire);
    uint8_t last_severity =
        last_backpressure_signal_severity_.load(std::memory_order_acquire);

    while (true) {
        const bool first = last == 0;
        const bool interval_elapsed = now_ns >= last + interval_ns;
        const bool escalation = severity > last_severity;

        if (!first && !interval_elapsed && !escalation) {
            return std::nullopt;
        }

        if (last_backpressure_signal_ns_.compare_exchange_weak(
                last, now_ns, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            last_backpressure_signal_severity_.store(
                severity, std::memory_order_release);
            return backpressure_signal_sequence_.fetch_add(
                       1, std::memory_order_acq_rel) +
                   1;
        }

        last_severity =
            last_backpressure_signal_severity_.load(std::memory_order_acquire);
    }
}
```

Add private helper:

```cpp
static uint8_t pressure_severity(MailboxPressureState state) noexcept {
    switch (state) {
        case MailboxPressureState::Normal:
            return 0;
        case MailboxPressureState::Recovering:
            return 1;
        case MailboxPressureState::SoftPressure:
            return 2;
        case MailboxPressureState::HardPressure:
            return 3;
    }
    return 0;
}
```

Add atomics to the private member list:

```cpp
std::atomic<uint64_t> last_backpressure_signal_ns_{0};
std::atomic<uint8_t> last_backpressure_signal_severity_{0};
std::atomic<uint64_t> backpressure_signal_sequence_{0};
```

- [ ] **Step 4: Run tests**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="BoundedMailboxTest.BackpressureSignalBudget*"
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp tests/unit/mailbox/test_bounded_mailbox.cpp
git commit -m "feat(mailbox): rate limit backpressure signals"
```

---

### Task 5: Add Backpressure Signal Protobuf Serialization

**Files:**
- Modify: `protos/hpactor/messages.proto`
- Modify: `include/hpactor/core/proto_type_registry.hpp`
- Modify: `src/core/proto_type_registry.cpp`
- Create: `include/hpactor/mailbox/backpressure_signal_serialization.hpp`
- Create: `src/mailbox/backpressure_signal_serialization.cpp`
- Modify: `src/CMakeLists.txt`
- Test: create `tests/unit/mailbox/test_backpressure_signal_serialization.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

- [ ] **Step 1: Add failing serialization test**

Create `tests/unit/mailbox/test_backpressure_signal_serialization.cpp`:

```cpp
#include <gtest/gtest.h>

#include <hpactor/mailbox/backpressure_signal_serialization.hpp>

using namespace hpactor;

TEST(BackpressureSignalSerializationTest, RoundTripPreservesFields) {
    mailbox::BackpressureSignal signal;
    signal.target = ActorAddress{endpoint_ops::parse_endpoint("127.0.0.1:1111"),
                                 ActorType{7}, ActorId{42}, 3};
    signal.sender = ActorAddress{endpoint_ops::parse_endpoint("127.0.0.1:2222"),
                                 ActorType{8}, ActorId{99}, 4};
    signal.reason = mailbox::BackpressureReason::ByteCapacity;
    signal.depth = 10;
    signal.capacity = 20;
    signal.bytes = 1000;
    signal.byte_capacity = 2000;
    signal.pressure_ratio = 0.5;
    signal.retry_after = std::chrono::milliseconds{250};
    signal.sequence = 123;

    auto encoded = mailbox::serialize_backpressure_signal(
        signal, mailbox::MailboxPressureState::SoftPressure);
    ASSERT_FALSE(encoded.empty());

    auto decoded = mailbox::deserialize_backpressure_signal(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->signal.target.id, ActorId{42});
    EXPECT_EQ(decoded->signal.sender.id, ActorId{99});
    EXPECT_EQ(decoded->signal.reason, mailbox::BackpressureReason::ByteCapacity);
    EXPECT_EQ(decoded->state, mailbox::MailboxPressureState::SoftPressure);
    EXPECT_EQ(decoded->signal.depth, 10u);
    EXPECT_EQ(decoded->signal.capacity, 20u);
    EXPECT_EQ(decoded->signal.bytes, 1000u);
    EXPECT_EQ(decoded->signal.byte_capacity, 2000u);
    EXPECT_EQ(decoded->signal.retry_after, std::chrono::milliseconds{250});
    EXPECT_EQ(decoded->signal.sequence, 123u);
}
```

Add the file to `tests/unit/mailbox/CMakeLists.txt`.

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build tests/unit/mailbox/test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="BackpressureSignalSerializationTest.*"
```

Expected: fail because helper header and protobuf message do not exist.

- [ ] **Step 3: Add protobuf message**

Append to `protos/hpactor/messages.proto`:

```proto
message BackpressureSignalMessage {
  PbActorAddress target = 1;
  PbActorAddress sender = 2;
  uint32 reason = 3;
  uint32 pressure_state = 4;
  uint32 depth = 5;
  uint32 capacity = 6;
  uint64 bytes = 7;
  uint64 byte_capacity = 8;
  double pressure_ratio = 9;
  uint64 retry_after_ms = 10;
  uint64 sequence = 11;
}
```

- [ ] **Step 4: Register system protobuf type**

In `include/hpactor/core/proto_type_registry.hpp`, forward-declare and add traits:

```cpp
class BackpressureSignalMessage;
HPACTOR_SYSTEM_MESSAGE(::hpactor::BackpressureSignalMessage,
                       TypeTag::BackpressureSignalTag)
```

In `src/core/proto_type_registry.cpp`, add:

```cpp
register_type<::hpactor::BackpressureSignalMessage>(
    TypeTag::BackpressureSignalTag,
    "hpactor.BackpressureSignalMessage");
```

- [ ] **Step 5: Add serialization helper header**

Create `include/hpactor/mailbox/backpressure_signal_serialization.hpp`:

```cpp
#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <optional>

namespace hpactor::mailbox {

struct DecodedBackpressureSignal {
    BackpressureSignal signal;
    MailboxPressureState state = MailboxPressureState::Normal;
};

[[nodiscard]] StreamBuffer
serialize_backpressure_signal(const BackpressureSignal& signal,
                              MailboxPressureState state);

[[nodiscard]] std::optional<DecodedBackpressureSignal>
deserialize_backpressure_signal(const StreamBuffer& payload);

} // namespace hpactor::mailbox
```

- [ ] **Step 6: Add serialization helper implementation**

Create `src/mailbox/backpressure_signal_serialization.cpp`:

```cpp
#include <hpactor/mailbox/backpressure_signal_serialization.hpp>

#include <hpactor/messages.pb.h>
#include <hpactor/net/frame.hpp>

namespace hpactor::mailbox {

namespace {

MailboxPressureState pressure_state_from_u32(uint32_t value) noexcept {
    switch (static_cast<MailboxPressureState>(value)) {
        case MailboxPressureState::Normal:
        case MailboxPressureState::SoftPressure:
        case MailboxPressureState::HardPressure:
        case MailboxPressureState::Recovering:
            return static_cast<MailboxPressureState>(value);
    }
    return MailboxPressureState::Normal;
}

BackpressureReason reason_from_u32(uint32_t value) noexcept {
    switch (static_cast<BackpressureReason>(value)) {
        case BackpressureReason::HighWatermark:
        case BackpressureReason::HardCapacity:
        case BackpressureReason::ByteCapacity:
        case BackpressureReason::OverflowPolicy:
        case BackpressureReason::NodeMemoryPressure:
            return static_cast<BackpressureReason>(value);
    }
    return BackpressureReason::HighWatermark;
}

} // namespace

StreamBuffer serialize_backpressure_signal(const BackpressureSignal& signal,
                                           MailboxPressureState state) {
    ::hpactor::BackpressureSignalMessage pb;
    net::to_proto(pb.mutable_target(), signal.target);
    net::to_proto(pb.mutable_sender(), signal.sender);
    pb.set_reason(static_cast<uint32_t>(signal.reason));
    pb.set_pressure_state(static_cast<uint32_t>(state));
    pb.set_depth(signal.depth);
    pb.set_capacity(signal.capacity);
    pb.set_bytes(signal.bytes);
    pb.set_byte_capacity(signal.byte_capacity);
    pb.set_pressure_ratio(signal.pressure_ratio);
    pb.set_retry_after_ms(
        static_cast<uint64_t>(signal.retry_after.count()));
    pb.set_sequence(signal.sequence);

    StreamBuffer out(pb.ByteSizeLong());
    if (!pb.SerializeToArray(out.data(), static_cast<int>(out.size()))) {
        out.clear();
    }
    return out;
}

std::optional<DecodedBackpressureSignal>
deserialize_backpressure_signal(const StreamBuffer& payload) {
    ::hpactor::BackpressureSignalMessage pb;
    if (!pb.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return std::nullopt;
    }

    DecodedBackpressureSignal decoded;
    decoded.signal.target = net::from_proto(pb.target());
    decoded.signal.sender = net::from_proto(pb.sender());
    decoded.signal.reason = reason_from_u32(pb.reason());
    decoded.state = pressure_state_from_u32(pb.pressure_state());
    decoded.signal.depth = pb.depth();
    decoded.signal.capacity = pb.capacity();
    decoded.signal.bytes = pb.bytes();
    decoded.signal.byte_capacity = pb.byte_capacity();
    decoded.signal.pressure_ratio = pb.pressure_ratio();
    decoded.signal.retry_after =
        std::chrono::milliseconds(pb.retry_after_ms());
    decoded.signal.sequence = pb.sequence();
    return decoded;
}

} // namespace hpactor::mailbox
```

- [ ] **Step 7: Add source to build**

In `src/CMakeLists.txt`, add:

```cmake
mailbox/backpressure_signal_serialization.cpp
```

next to other mailbox sources.

- [ ] **Step 8: Run tests**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build tests/unit/mailbox/test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="BackpressureSignalSerializationTest.*"
```

Expected: pass.

- [ ] **Step 9: Commit**

```bash
git add protos/hpactor/messages.proto include/hpactor/core/proto_type_registry.hpp src/core/proto_type_registry.cpp include/hpactor/mailbox/backpressure_signal_serialization.hpp src/mailbox/backpressure_signal_serialization.cpp src/CMakeLists.txt tests/unit/mailbox/test_backpressure_signal_serialization.cpp tests/unit/mailbox/CMakeLists.txt
git commit -m "feat(mailbox): serialize backpressure signals"
```

---

### Task 6: Emit Local Signals with Mode and Rate Limit

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Test: `tests/integration/actor/test_backpressure_signals.cpp`
- Test: `tests/integration/actor/test_actor_system_backpressure.cpp`

- [ ] **Step 1: Add failing local signal tests**

Append to `tests/integration/actor/test_backpressure_signals.cpp`:

```cpp
TEST_F(BackpressureSignalsTest, DisabledModeSuppressesLocalSignal) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 2;
    cfg.mailbox.high_watermark = 0.50;
    cfg.mailbox.backpressure_mode = mailbox::BackpressureMode::Disabled;
    ActorSystem system(cfg);

    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();

    auto* sender_local = static_cast<LocalActor*>(sender.get().get());
    bool signaled = false;
    sender_local->context()->on_backpressure(
        [&](const mailbox::BackpressureSignal&) { signaled = true; });

    auto result = sender_local->context()->try_send(
        target.address(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_TRUE(result.accepted());
    EXPECT_FALSE(signaled);
}

TEST_F(BackpressureSignalsTest, HardCapacityFailureSignalsRetryAfter) {
    auto sender = system_->spawn<EventBasedActor>();
    auto target = system_->spawn<EventBasedActor>();

    auto* sender_local = static_cast<LocalActor*>(sender.get().get());
    auto* sender_ctx = sender_local->context();

    mailbox::BackpressureSignal observed;
    bool signaled = false;
    sender_ctx->on_backpressure([&](const mailbox::BackpressureSignal& signal) {
        observed = signal;
        signaled = true;
    });

    ASSERT_TRUE(sender_ctx
                    ->try_send(target.address(),
                               TypedMessage(TypeTag::User, StreamBuffer{1}))
                    .accepted());
    auto rejected = sender_ctx->try_send(
        target.address(), TypedMessage(TypeTag::User, StreamBuffer{2}));

    EXPECT_FALSE(rejected.accepted());
    EXPECT_TRUE(signaled);
    EXPECT_EQ(observed.reason, mailbox::BackpressureReason::HardCapacity);
    EXPECT_GT(observed.retry_after.count(), 0);
}
```

- [ ] **Step 2: Run tests and verify failure**

Run:

```bash
./build/tests/integration/actor/test_integration_actor --gtest_filter="BackpressureSignalsTest.DisabledModeSuppressesLocalSignal:BackpressureSignalsTest.HardCapacityFailureSignalsRetryAfter"
```

Expected: first test fails because mode is ignored; second fails because hard rejection does not signal.

- [ ] **Step 3: Declare ActorSystem helper methods**

In `include/hpactor/core/actor_system.hpp`, add private declarations near delivery helpers:

```cpp
void maybe_emit_backpressure_signal(
    ActorId target, const mailbox::MailboxEnvelopeMeta& meta,
    const mailbox::EnqueueResult& result,
    mailbox::MPSCActorMailbox<TypedMessage>* mailbox,
    bool emit_requested);

void emit_local_backpressure_signal(
    const mailbox::BackpressureSignal& signal,
    mailbox::MailboxPressureState state);
```

- [ ] **Step 4: Implement local signal emission**

In `src/actor/actor_system.cpp`, replace the existing soft-pressure-only block with:

```cpp
maybe_emit_backpressure_signal(target, meta, result, mailbox,
                               options.emit_backpressure);
```

Add helper implementation:

```cpp
namespace {

bool local_signal_enabled(mailbox::BackpressureMode mode) noexcept {
    return mode == mailbox::BackpressureMode::LocalSignal ||
           mode == mailbox::BackpressureMode::LocalAndRemoteSignal;
}

bool pressure_result_should_signal(const mailbox::EnqueueResult& result) noexcept {
    if (result.code == mailbox::EnqueueResultCode::AcceptedWithSoftPressure) {
        return true;
    }
    return !result.accepted() && result.retryable();
}

} // namespace

void ActorSystem::maybe_emit_backpressure_signal(
    ActorId target, const mailbox::MailboxEnvelopeMeta& meta,
    const mailbox::EnqueueResult& result,
    mailbox::MPSCActorMailbox<TypedMessage>* mailbox,
    bool emit_requested) {
    if (!emit_requested || mailbox == nullptr ||
        !pressure_result_should_signal(result)) {
        return;
    }

    const auto mode = mailbox->config().backpressure_mode;
    if (!local_signal_enabled(mode)) {
        return;
    }

    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    auto sequence =
        mailbox->try_acquire_backpressure_signal(now_ns, result.pressure_state);
    if (!sequence.has_value()) {
        return;
    }

    mailbox::BackpressureSignal signal;
    signal.target = ActorAddress{endpoint_, ActorType{0}, target, 0};
    signal.sender = meta.sender;
    signal.reason = result.pressure_reason;
    signal.depth = result.depth;
    signal.capacity = result.capacity;
    signal.bytes = result.bytes;
    signal.byte_capacity = result.byte_capacity;
    signal.pressure_ratio = result.pressure_ratio;
    signal.retry_after = result.retry_after;
    signal.sequence = sequence.value();

    emit_local_backpressure_signal(signal, result.pressure_state);
}

void ActorSystem::emit_local_backpressure_signal(
    const mailbox::BackpressureSignal& signal,
    mailbox::MailboxPressureState state) {
    if (metrics_ring_buffer_) [[unlikely]] {
        metrics::MetricEvent evt{};
        evt.actor_id = signal.target.id;
        evt.event_type = metrics::MetricEventType::kBackpressureSignal;
        evt.code = static_cast<uint8_t>(signal.reason);
        evt.aux = static_cast<uint8_t>(state);
        evt.value_hi = 1;
        metrics_ring_buffer_->try_push(evt);
    }

    if (logger_ && state == mailbox::MailboxPressureState::HardPressure) {
        HPACTOR_LOG_WARNING(
            log::LogCategory::kMailbox, signal.target.id, 0,
            "backpressure_signal_sent",
            log::field("sender", signal.sender.id.value()),
            log::field("depth", static_cast<uint64_t>(signal.depth)),
            log::field("capacity", static_cast<uint64_t>(signal.capacity)),
            log::field("retry_after_ms",
                       static_cast<uint64_t>(signal.retry_after.count())));
    }

    signal_backpressure(signal);
}
```

Adjust access specifiers or declarations if the helper is placed in a different section.

- [ ] **Step 5: Run tests**

Run:

```bash
./build/tests/integration/actor/test_integration_actor --gtest_filter="BackpressureSignalsTest.*:ActorSystemBackpressureTest.*"
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/integration/actor/test_backpressure_signals.cpp tests/integration/actor/test_actor_system_backpressure.cpp
git commit -m "feat(mailbox): emit local backpressure signals"
```

---

### Task 7: Emit and Receive Remote Backpressure Signals

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/actor/test_actor_system_backpressure.cpp`
- Create: `tests/integration/actor/test_remote_backpressure_signals.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

- [ ] **Step 1: Add remote signal receive test**

Create `tests/integration/actor/test_remote_backpressure_signals.cpp`:

```cpp
#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/backpressure_signal_serialization.hpp>
#include <hpactor/net/frame.hpp>

using namespace hpactor;

TEST(RemoteBackpressureSignalsTest, DeliverRemoteSignalInvokesLocalHandler) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:9001");
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto producer = system.spawn<EventBasedActor>();
    auto* producer_local = static_cast<LocalActor*>(producer.get().get());
    ASSERT_NE(producer_local->context(), nullptr);

    mailbox::BackpressureSignal observed;
    bool signaled = false;
    producer_local->context()->on_backpressure(
        [&](const mailbox::BackpressureSignal& signal) {
            observed = signal;
            signaled = true;
        });

    mailbox::BackpressureSignal signal;
    signal.sender = producer.address();
    signal.target = ActorAddress{endpoint_ops::parse_endpoint("127.0.0.1:9002"),
                                 ActorType{0}, ActorId{777}, 0};
    signal.reason = mailbox::BackpressureReason::HardCapacity;
    signal.depth = 10;
    signal.capacity = 10;
    signal.pressure_ratio = 1.0;
    signal.retry_after = std::chrono::milliseconds{200};
    signal.sequence = 44;

    net::WireFrame frame;
    net::to_proto(frame.pb_frame.mutable_sender(), signal.target);
    net::to_proto(frame.pb_frame.mutable_receiver(), signal.sender);
    frame.pb_frame.set_type_tag(
        static_cast<uint32_t>(TypeTag::BackpressureSignalTag));
    frame.pb_frame.set_message_id(signal.sequence);
    auto payload = mailbox::serialize_backpressure_signal(
        signal, mailbox::MailboxPressureState::HardPressure);
    frame.pb_frame.set_payload(reinterpret_cast<const char*>(payload.data()),
                               payload.size());

    system.deliver_remote(frame);

    EXPECT_TRUE(signaled);
    EXPECT_EQ(observed.sender.id, producer.id());
    EXPECT_EQ(observed.target.id, ActorId{777});
    EXPECT_EQ(observed.reason, mailbox::BackpressureReason::HardCapacity);
    EXPECT_EQ(observed.retry_after, std::chrono::milliseconds{200});
}
```

Add this file to `tests/integration/actor/CMakeLists.txt`.

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
ninja -C build tests/integration/actor/test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter="RemoteBackpressureSignalsTest.*"
```

Expected: fail because `deliver_remote()` does not intercept `BackpressureSignalTag`.

- [ ] **Step 3: Add ActorSystem remote helper declarations**

In `include/hpactor/core/actor_system.hpp`, declare:

```cpp
void emit_remote_backpressure_signal(
    const mailbox::BackpressureSignal& signal,
    mailbox::MailboxPressureState state);

bool handle_remote_backpressure_signal(const net::WireFrame& frame);

using BackpressureSignalWireSink =
    std::function<bool(const ActorAddress&, const StreamBuffer&)>;
void set_backpressure_signal_wire_sink_for_test(
    BackpressureSignalWireSink sink);
```

Add this private member near `transport_`:

```cpp
BackpressureSignalWireSink backpressure_signal_wire_sink_for_test_;
```

- [ ] **Step 4: Intercept incoming remote signal**

In `src/actor/actor_system.cpp`, include:

```cpp
#include <hpactor/mailbox/backpressure_signal_serialization.hpp>
```

At the top of `ActorSystem::deliver_remote()`:

```cpp
if (static_cast<TypeTag>(frame.pb_frame.type_tag()) ==
    TypeTag::BackpressureSignalTag) {
    (void)handle_remote_backpressure_signal(frame);
    return;
}
```

Implement:

```cpp
bool ActorSystem::handle_remote_backpressure_signal(const net::WireFrame& frame) {
    StreamBuffer payload(frame.pb_frame.payload().begin(),
                         frame.pb_frame.payload().end());
    auto decoded = mailbox::deserialize_backpressure_signal(payload);
    if (!decoded.has_value()) {
        return false;
    }
    signal_backpressure(decoded->signal);
    return true;
}
```

- [ ] **Step 5: Emit outbound remote signal**

Update `maybe_emit_backpressure_signal()` from Task 6 to check remote mode:

```cpp
bool remote_signal_enabled(mailbox::BackpressureMode mode) noexcept {
    return mode == mailbox::BackpressureMode::RemoteSignal ||
           mode == mailbox::BackpressureMode::LocalAndRemoteSignal;
}
```

After constructing `signal`, choose direction:

```cpp
const bool sender_is_remote = signal.sender.endpoint != endpoint_;
if (sender_is_remote && remote_signal_enabled(mode)) {
    emit_remote_backpressure_signal(signal, result.pressure_state);
    return;
}
if (!sender_is_remote && local_signal_enabled(mode)) {
    emit_local_backpressure_signal(signal, result.pressure_state);
}
```

Implement remote frame sending:

```cpp
void ActorSystem::emit_remote_backpressure_signal(
    const mailbox::BackpressureSignal& signal,
    mailbox::MailboxPressureState state) {
    auto payload = mailbox::serialize_backpressure_signal(signal, state);
    if (payload.empty()) {
        return;
    }

    net::WireFrame frame;
    net::to_proto(frame.pb_frame.mutable_sender(), signal.target);
    net::to_proto(frame.pb_frame.mutable_receiver(), signal.sender);
    frame.pb_frame.set_type_tag(
        static_cast<uint32_t>(TypeTag::BackpressureSignalTag));
    frame.pb_frame.set_message_id(signal.sequence);
    frame.pb_frame.set_payload(reinterpret_cast<const char*>(payload.data()),
                               payload.size());

    bool sent = false;
    auto encoded = frame.encode();
    if (backpressure_signal_wire_sink_for_test_) {
        sent = backpressure_signal_wire_sink_for_test_(signal.sender, encoded);
    } else if (transport_) {
        sent = transport_->try_send(signal.sender, encoded);
    }

    if (sent) {
        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = signal.target.id;
            evt.event_type = metrics::MetricEventType::kBackpressureSignal;
            evt.code = static_cast<uint8_t>(signal.reason);
            evt.aux = static_cast<uint8_t>(state);
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }
    }
}
```

- [ ] **Step 6: Add remote emission test**

Implement the test hook in `src/actor/actor_system.cpp`:

```cpp
void ActorSystem::set_backpressure_signal_wire_sink_for_test(
    BackpressureSignalWireSink sink) {
    backpressure_signal_wire_sink_for_test_ = std::move(sink);
}
```

Add this test to `test_remote_backpressure_signals.cpp`:

```cpp
TEST(RemoteBackpressureSignalsTest, RemoteSenderReceivesControlFrame) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:9001");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 1;
    cfg.mailbox.backpressure_mode = mailbox::BackpressureMode::RemoteSignal;
    ActorSystem system(cfg);

    bool sent = false;
    ActorAddress wire_receiver;
    StreamBuffer wire_payload;
    system.set_backpressure_signal_wire_sink_for_test(
        [&](const ActorAddress& receiver, const StreamBuffer& encoded) {
            sent = true;
            wire_receiver = receiver;
            wire_payload = encoded;
            return true;
        });

    auto target = system.spawn<EventBasedActor>();

    TypedMessage first(TypeTag::User, StreamBuffer{1});
    first.set_sender_address(ActorAddress{
        endpoint_ops::parse_endpoint("127.0.0.1:9002"), ActorType{0},
        ActorId{55}, 0});
    ASSERT_TRUE(system.try_deliver_local(target.id(), std::move(first)).accepted());

    TypedMessage second(TypeTag::User, StreamBuffer{2});
    second.set_sender_address(ActorAddress{
        endpoint_ops::parse_endpoint("127.0.0.1:9002"), ActorType{0},
        ActorId{55}, 0});
    auto rejected = system.try_deliver_local(target.id(), std::move(second));

    EXPECT_FALSE(rejected.accepted());
    EXPECT_TRUE(sent);
    EXPECT_EQ(wire_receiver.id, ActorId{55});

    auto frame = net::WireFrame::decode(wire_payload);
    EXPECT_EQ(static_cast<TypeTag>(frame.pb_frame.type_tag()),
              TypeTag::BackpressureSignalTag);
    StreamBuffer payload(frame.pb_frame.payload().begin(),
                         frame.pb_frame.payload().end());
    auto decoded = mailbox::deserialize_backpressure_signal(payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->signal.sender.id, ActorId{55});
    EXPECT_EQ(decoded->signal.target.id, target.id());
    EXPECT_EQ(decoded->signal.reason, mailbox::BackpressureReason::HardCapacity);
    EXPECT_GT(decoded->signal.retry_after.count(), 0);
}
```

- [ ] **Step 7: Run tests**

Run:

```bash
ninja -C build tests/integration/actor/test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter="RemoteBackpressureSignalsTest.*:BackpressureSignalsTest.*"
```

Expected: pass.

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/integration/actor/test_remote_backpressure_signals.cpp tests/integration/actor/CMakeLists.txt
git commit -m "feat(mailbox): propagate remote backpressure signals"
```

---

### Task 8: Expose Pressure State Through CLI

**Files:**
- Modify: `include/hpactor/cli/cli_types.hpp`
- Modify: `protos/hpactor/cli_messages.proto`
- Modify: `src/actor/event_based_actor.cpp`
- Modify: `src/cli/commands/actor_commands.cpp`
- Test: relevant CLI or actor integration tests

- [ ] **Step 1: Add expected CLI snapshot fields**

Extend `protos/hpactor/cli_messages.proto` `MailboxSnapshot`:

```proto
message MailboxSnapshot {
    uint32 depth = 1;
    uint64 total_enqueued = 2;
    uint64 total_dequeued = 3;
    uint64 max_depth = 4;
    uint32 high_priority_depth = 5;
    uint32 capacity = 6;
    uint64 queued_bytes = 7;
    uint64 byte_capacity = 8;
    uint32 pressure_ratio_ppm = 9;
    uint64 total_rejected = 10;
    uint64 total_dropped = 11;
    uint64 total_dead_letters = 12;
    string pressure_state = 13;
    string overflow_policy = 14;
}
```

This is protobuf-compatible because it only appends fields.

- [ ] **Step 2: Populate protobuf reply**

In `src/actor/event_based_actor.cpp`, replace the mailbox reply block with:

```cpp
if (req.include_mailbox()) {
    auto ms = mailbox_snapshot();
    auto* pb_mbox = reply.mutable_mailbox();
    pb_mbox->set_depth(ms.depth);
    pb_mbox->set_total_enqueued(ms.total_enqueued);
    pb_mbox->set_total_dequeued(ms.total_dequeued);
    pb_mbox->set_max_depth(ms.max_depth);
    pb_mbox->set_high_priority_depth(ms.high_priority_depth);
    pb_mbox->set_capacity(ms.capacity);
    pb_mbox->set_queued_bytes(ms.queued_bytes);
    pb_mbox->set_byte_capacity(ms.byte_capacity);
    pb_mbox->set_pressure_ratio_ppm(ms.pressure_ratio_ppm);
    pb_mbox->set_total_rejected(ms.total_rejected);
    pb_mbox->set_total_dropped(ms.total_dropped);
    pb_mbox->set_total_dead_letters(ms.total_dead_letters);
    pb_mbox->set_pressure_state(ms.pressure_state);
    pb_mbox->set_overflow_policy(ms.overflow_policy);
}
```

- [ ] **Step 3: Print fields in actor command**

In `src/cli/commands/actor_commands.cpp`, extend the mailbox display:

```cpp
if (reply->has_mailbox()) {
    const auto& mailbox = reply->mailbox();
    kv["Mailbox depth"] =
        std::to_string(mailbox.depth()) + "/" +
        std::to_string(mailbox.capacity());
    kv["Mailbox bytes"] =
        std::to_string(mailbox.queued_bytes()) + "/" +
        std::to_string(mailbox.byte_capacity());
    kv["Mailbox pressure"] = mailbox.pressure_state();
    kv["Mailbox overflow"] = mailbox.overflow_policy();
    kv["Mailbox rejected"] = std::to_string(mailbox.total_rejected());
    kv["Mailbox dropped"] = std::to_string(mailbox.total_dropped());
    kv["Mailbox dead letters"] =
        std::to_string(mailbox.total_dead_letters());
    kv["Mailbox max"] = std::to_string(mailbox.max_depth());
}
```

- [ ] **Step 4: Add snapshot integration test**

Add this test to `tests/integration/actor/test_actor_system_backpressure.cpp`.
It verifies the mailbox snapshot fields that `InspectStateReply` copies into
the CLI protobuf:

```cpp
TEST_F(ActorSystemBackpressureTest, MailboxSnapshotExposesPressureForCli) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 2;
    cfg.mailbox.high_watermark = 0.50;
    ActorSystem system(cfg);

    auto actor = system.spawn<EventBasedActor>();
    auto* event_actor = static_cast<EventBasedActor*>(actor.get().get());
    ASSERT_NE(event_actor, nullptr);

    auto result = system.try_deliver_local(
        actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    ASSERT_TRUE(result.accepted());

    auto snap = event_actor->mailbox_snapshot();
    EXPECT_EQ(snap.depth, 1u);
    EXPECT_EQ(snap.capacity, 2u);
    EXPECT_EQ(snap.pressure_state, "soft_pressure");
    EXPECT_EQ(snap.overflow_policy, "reject_newest");
    EXPECT_EQ(snap.total_enqueued, 1u);
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/integration/cli/test_integration_cli tests/integration/actor/test_integration_actor
ctest -R "Cli|InspectState|Backpressure" --output-on-failure
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/cli/cli_types.hpp protos/hpactor/cli_messages.proto src/actor/event_based_actor.cpp src/cli/commands/actor_commands.cpp tests/integration/cli tests/integration/actor
git commit -m "feat(cli): expose mailbox pressure state"
```

---

### Task 9: Verify Metrics and Logs

**Files:**
- Modify: `tests/integration/metrics/test_metrics_aggregator.cpp`
- Modify: `tests/integration/actor/test_backpressure_signals.cpp`

- [ ] **Step 1: Strengthen metrics aggregator test**

In `tests/integration/metrics/test_metrics_aggregator.cpp`, add:

```cpp
TEST_F(MetricsAggregatorTest, BackpressureSignalEventCarriesReasonAndState) {
    MetricEvent bp{};
    bp.actor_id = hpactor::ActorId{77};
    bp.event_type = MetricEventType::kBackpressureSignal;
    bp.code =
        static_cast<uint8_t>(hpactor::mailbox::BackpressureReason::HardCapacity);
    bp.aux =
        static_cast<uint8_t>(hpactor::mailbox::MailboxPressureState::HardPressure);
    bp.value_hi = 1;

    agg().begin_drain();
    agg().on_event(bp);
    agg().end_drain();

    auto snapshot = registry().snapshot();
    bool found = false;
    for (const auto& family : snapshot.families) {
        if (family.name == "hpactor_backpressure_signals_total") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}
```

- [ ] **Step 2: Add signal metric integration assertion**

In `tests/integration/actor/test_backpressure_signals.cpp`, extend
`HardCapacityFailureSignalsRetryAfter` to assert that the signal carries fields
used by metrics and logs:

```cpp
EXPECT_EQ(observed.target.id, target.id());
EXPECT_EQ(observed.sender.id, sender.id());
EXPECT_EQ(observed.depth, 1u);
EXPECT_EQ(observed.capacity, 2u);
EXPECT_GE(observed.pressure_ratio, 0.5);
EXPECT_GT(observed.sequence, 0u);
```

- [ ] **Step 3: Run tests**

Run:

```bash
./build/tests/integration/metrics/test_integration_metrics --gtest_filter="*Backpressure*"
./build/tests/integration/actor/test_integration_actor --gtest_filter="BackpressureSignalsTest.*"
```

Expected: pass.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/metrics/test_metrics_aggregator.cpp tests/integration/actor/test_backpressure_signals.cpp
git commit -m "test(mailbox): cover backpressure observability"
```

---

### Task 10: Full Verification

**Files:**
- No source edits unless verification exposes a defect.

- [ ] **Step 1: Configure**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Expected: configuration completes without protobuf or CMake errors.

- [ ] **Step 2: Build**

Run:

```bash
ninja -C build
```

Expected: all targets build.

- [ ] **Step 3: Run focused tests**

Run:

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*Backpressure*:*Pressure*:*ByteBudget*:*BoundedMailbox*"
./build/tests/integration/actor/test_integration_actor --gtest_filter="*Backpressure*:RemoteBackpressureSignalsTest.*"
./build/tests/integration/metrics/test_integration_metrics --gtest_filter="*Backpressure*"
ctest -R "Backpressure|Mailbox|InspectState" --output-on-failure
```

Expected: all focused tests pass.

- [ ] **Step 4: Run full test suite**

Run:

```bash
ctest --output-on-failure --parallel 8
```

Expected: all configured tests pass.

- [ ] **Step 5: Inspect worktree**

Run:

```bash
git status --short
git branch --show-current
```

Expected: branch is `codex/issue-24-backpressure-design` or the implementation branch derived from it; only intended MBX-003 files are modified before the final commit.

- [ ] **Step 6: Final commit**

```bash
git add include/hpactor/mailbox include/hpactor/config src/config/parsers src/mailbox protos/hpactor include/hpactor/core src/core include/hpactor/core src/actor include/hpactor/cli src/cli tests
git commit -m "feat(mailbox): add upstream backpressure signalling"
```

---

## Self-Review

Spec coverage:

- low/high/critical watermarks: Tasks 1 and 2.
- local signals: Task 6.
- remote retry-after/slow-down signals: Tasks 5 and 7.
- rate limiting: Task 4.
- metrics, CLI, logs: Tasks 6, 8, and 9.
- tests: Tasks 1 through 10.

No unresolved sections remain. Type names used across tasks match the design spec: `BackpressureSignalMessage`, `BackpressureSignal`, `MailboxPressureState`, `BackpressureReason`, and `BackpressureSignalTag`.
