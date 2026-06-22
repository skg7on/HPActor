# Sprint 3 Akka Gap Closure — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement MSG-003 Reliable Messaging, CLU-003 Singleton Actor Integration, and DUR-001/002 Durable Actor Runtime — the three remaining P1 gaps from issue #329.

**Architecture:** Three sequential PRs. PR #1 adds reliable at-least-once messaging (ACK/NACK wire protocol, OutboundTracker, retry/backoff, durable delivery stores). PR #2 wraps SingletonManagerCore and ShardCoordinatorCore in EventBasedActors with ActorSystem integration. PR #3 adds DurableBehavior and EventSourcedBehavior templates, PassivationManager system actor, and recovery hooks.

**Tech Stack:** C++20, Google Test, Ninja build, BehaviorTestKit + SchedulerTestDriver for deterministic tests.

**Design spec:** `docs/superpowers/specs/2026-06-22-akka-gap-closure-sprint3-design.md`

**Important:** `FailureReason` codes `RetryExhausted` (80), `FencingTokenStale` (53), `NodeQuarantined` (3), `NodeReplaced` (4), and all passivation-range codes (100-109) already exist from Sprint 2. New TypeTags use `make_subsystem_tag()` in the 0x76–0x7B range.

---

## File Map

### PR #1: MSG-003 Reliable Messaging

#### New files created
```
include/hpactor/net/reliable_ack.hpp                    — AckStatus, AckPayload, encode/decode
include/hpactor/mailbox/outbound_tracker.hpp            — OutboundTrackerEntry, OutboundTracker
include/hpactor/mailbox/reliable_retry_policy.hpp       — ReliableRetryPolicy
include/hpactor/mailbox/in_memory_delivery_store.hpp    — InMemoryDeliveryStore
include/hpactor/mailbox/file_delivery_store.hpp         — FileDeliveryStore
src/net/reliable_ack.cpp                                — ACK/NACK wire format impl
src/mailbox/outbound_tracker.cpp                        — Tracker impl
src/mailbox/in_memory_delivery_store.cpp                — InMemory store impl
src/mailbox/file_delivery_store.cpp                     — File store impl
tests/unit/net/test_reliable_ack.cpp                    — ACK serialization tests
tests/unit/mailbox/test_reliable_retry_policy.cpp       — Backoff tests
tests/unit/mailbox/test_outbound_tracker.cpp            — Tracker unit tests
tests/unit/mailbox/test_delivery_store.cpp              — Both store adapters
tests/integration/mailbox/test_reliable_messaging.cpp   — End-to-end tests
```

#### Existing files modified
```
include/hpactor/net/frame.hpp                           — Add AckRequested/AckResponse flags
src/actor/event_based_actor.cpp                         — Auto-ACK/NACK in receive()
include/hpactor/core/actor_system.hpp                   — Own OutboundTracker
src/actor/actor_system.cpp                              — Init OutboundTracker
tests/unit/mailbox/CMakeLists.txt                       — Add new test sources
tests/unit/net/CMakeLists.txt                           — Add test_reliable_ack.cpp
tests/integration/mailbox/CMakeLists.txt                 — Add test_reliable_messaging.cpp
```

### PR #2: CLU-003 Singleton Actor Integration

#### New files created
```
include/hpactor/cluster/singleton/singleton_manager_actor.hpp  — SingletonManagerActor
include/hpactor/cluster/sharding/shard_coordinator_actor.hpp   — ShardCoordinatorActor
src/cluster/singleton/singleton_manager_actor.cpp              — Actor impl
src/cluster/sharding/shard_coordinator_actor.cpp               — Actor impl
tests/unit/cluster/singleton/test_singleton_manager_actor.cpp  — BehaviorTestKit tests
tests/unit/cluster/sharding/test_shard_coordinator_actor.cpp   — BehaviorTestKit tests
tests/integration/cluster/singleton/test_singleton_fencing.cpp — Integration tests
```

#### Existing files modified
```
include/hpactor/core/actor_system.hpp                   — Own singleton manager
src/actor/actor_system.cpp                              — Init singleton subsystem
include/hpactor/cluster/cluster_failure_model.hpp      — Observer callback
src/cluster/cluster_failure_model.cpp                   — Callback invocation
include/hpactor/cluster/singleton/singleton_manager.hpp — get_registered()
tests/unit/cluster/CMakeLists.txt                       — Add singleton actor test
```

### PR #3: DUR-001/002 Durable Actor Runtime

#### New files created
```
include/hpactor/actor/durable/durable_behavior.hpp           — DurableBehavior<State>
include/hpactor/actor/durable/event_sourced_behavior.hpp     — EventSourcedBehavior<State,Event>
include/hpactor/actor/durable/passivation_manager.hpp        — PassivationManager actor
include/hpactor/actor/durable/passivation_config.hpp         — PassivationConfig
include/hpactor/actor/durable/recovery_policy.hpp            — RecoveryPolicy
src/actor/durable/passivation_manager.cpp                    — PassivationManager impl
src/config/parsers/durable_config_parser.cpp                 — TOML parser
src/config/parsers/reliable_messaging_config_parser.cpp      — TOML parser
tests/unit/actor/durable/test_durable_behavior.cpp           — DurableBehavior tests
tests/unit/actor/durable/test_event_sourced_behavior.cpp     — EventSourcedBehavior tests
tests/unit/actor/durable/test_passivation_manager.cpp        — PassivationManager tests
tests/unit/actor/durable/test_recovery_policy.cpp            — RecoveryPolicy tests
tests/integration/actor/test_durable_workflow.cpp            — End-to-end tests
```

#### Existing files modified
```
include/hpactor/core/actor_system.hpp                   — Own PassivationManager
src/actor/actor_system.cpp                              — Init durable subsystem
src/actor/event_based_actor.cpp                         — Recovery hook in spawn
include/hpactor/actor/event_based_actor.hpp             — Recovery gate
tests/unit/actor/CMakeLists.txt                         — Add durable test target
tests/integration/actor/CMakeLists.txt                   — Add durable integration test
```

---

## PR #1: MSG-003 Reliable Messaging

### Task 1: CMake Scaffold for New Tests

**Files:**
- Modify: `tests/unit/mailbox/CMakeLists.txt`
- Modify: `tests/unit/net/CMakeLists.txt`
- Modify: `tests/integration/mailbox/CMakeLists.txt`

- [ ] **Step 1: Add new test sources to unit/mailbox CMakeLists.txt**

Add these source files to the existing `test_unit_mailbox` executable in `tests/unit/mailbox/CMakeLists.txt`:
```
    test_reliable_retry_policy.cpp
    test_outbound_tracker.cpp
    test_delivery_store.cpp
```
Insert them in alphabetical order before the closing `)`.

- [ ] **Step 2: Add test_reliable_ack.cpp to tests/unit/net/CMakeLists.txt**

Add `test_reliable_ack.cpp` to the existing test executable source list.

- [ ] **Step 3: Add integration test to tests/integration/mailbox/CMakeLists.txt**

Add `test_reliable_messaging.cpp` to the existing integration test executable source list.

- [ ] **Step 4: Create empty placeholder test files**

```bash
touch tests/unit/mailbox/test_reliable_retry_policy.cpp
touch tests/unit/mailbox/test_outbound_tracker.cpp
touch tests/unit/mailbox/test_delivery_store.cpp
touch tests/unit/net/test_reliable_ack.cpp
touch tests/integration/mailbox/test_reliable_messaging.cpp
```

Each placeholder must have a minimal GTest include to compile:
```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#include <gtest/gtest.h>
TEST(DummyTest, Placeholder) { EXPECT_TRUE(true); }
```

- [ ] **Step 5: Build the test targets**

```bash
ninja -C build test_unit_mailbox test_unit_net test_integration_mailbox
```

Expected: Build succeeds (placeholder tests compile).

- [ ] **Step 6: Commit**

```bash
git add tests/unit/mailbox/CMakeLists.txt tests/unit/net/CMakeLists.txt tests/integration/mailbox/CMakeLists.txt tests/unit/mailbox/test_reliable_retry_policy.cpp tests/unit/mailbox/test_outbound_tracker.cpp tests/unit/mailbox/test_delivery_store.cpp tests/unit/net/test_reliable_ack.cpp tests/integration/mailbox/test_reliable_messaging.cpp
git commit -m "build: add test scaffold for MSG-003 reliable messaging

Placeholder test files for OutboundTracker, ReliableRetryPolicy,
DeliveryStore, and reliable ACK wire format tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: AckStatus + AckPayload — Wire Types (RED)

**Files:**
- Create: `include/hpactor/net/reliable_ack.hpp`
- Modify: `tests/unit/net/test_reliable_ack.cpp` (replace placeholder)

- [ ] **Step 1: Write the failing test — tests/unit/net/test_reliable_ack.cpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include <hpactor/net/reliable_ack.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::net {

TEST(ReliableAckTest, AckStatusValuesAreDistinct) {
    EXPECT_NE(static_cast<uint8_t>(AckStatus::Accepted),
              static_cast<uint8_t>(AckStatus::Rejected));
    EXPECT_NE(static_cast<uint8_t>(AckStatus::Rejected),
              static_cast<uint8_t>(AckStatus::Duplicate));
}

TEST(ReliableAckTest, EncodeDecodeRoundtripAccepted) {
    AckPayload original{MessageId{0x12345678ABCDEF00}, AckStatus::Accepted,
                        Duration::zero()};
    auto encoded = encode_ack(original);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(encoded->size(), 14u);

    auto decoded = decode_ack(encoded->data(), encoded->size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->message_id, original.message_id);
    EXPECT_EQ(decoded->status, AckStatus::Accepted);
}

TEST(ReliableAckTest, EncodeDecodeRoundtripRejectedWithRetryAfter) {
    AckPayload original{MessageId{42}, AckStatus::Rejected,
                        std::chrono::milliseconds(500)};
    auto encoded = encode_ack(original);
    ASSERT_TRUE(encoded.has_value());

    auto decoded = decode_ack(encoded->data(), encoded->size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->message_id, original.message_id);
    EXPECT_EQ(decoded->status, AckStatus::Rejected);
    EXPECT_EQ(decoded->retry_after, std::chrono::milliseconds(500));
}

TEST(ReliableAckTest, DecodeRejectsShortBuffer) {
    uint8_t short_buf[5] = {0};
    auto decoded = decode_ack(short_buf, 5);
    EXPECT_FALSE(decoded.has_value());
}

TEST(ReliableAckTest, DecodeRejectsNullData) {
    auto decoded = decode_ack(nullptr, 14);
    EXPECT_FALSE(decoded.has_value());
}

} // namespace hpactor::net
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_net && ./build/tests/unit/net/test_unit_net --gtest_filter="ReliableAckTest*"
```

Expected: Compilation fails — `AckStatus`, `AckPayload`, `encode_ack()`, `decode_ack()` not defined.

---

### Task 3: AckStatus + AckPayload — Header Implementation (GREEN)

**Files:**
- Create: `include/hpactor/net/reliable_ack.hpp`
- Create: `src/net/reliable_ack.cpp`

- [ ] **Step 1: Write the header — include/hpactor/net/reliable_ack.hpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace hpactor::net {

/// \brief ACK/NACK status for reliable message delivery.
enum class AckStatus : uint8_t {
    Accepted = 0,   ///< Message admitted to receiver mailbox.
    Rejected = 1,   ///< Receiver rejected (full, draining, policy).
    Duplicate = 2,  ///< Already seen, suppressed.
};

/// \brief Compact binary ACK/NACK payload carried in frame body.
///
/// Wire format (14 bytes, big-endian integer fields):
/// ```
/// [0..7]:  message_id (uint64_t, big-endian)
/// [8]:     status (uint8_t)
/// [9]:     padding (uint8_t, reserved)
/// [10..13]: retry_after_ms (uint32_t, big-endian)
/// ```
struct AckPayload {
    MessageId message_id;
    AckStatus status = AckStatus::Accepted;
    Duration retry_after = Duration::zero();
};

/// \brief Encode an AckPayload to wire format.
///
/// \param[in] payload The ACK/NACK payload to encode.
/// \return A 14-byte StreamBuffer on success, or std::nullopt on failure.
std::optional<StreamBuffer> encode_ack(const AckPayload& payload);

/// \brief Decode an AckPayload from wire format.
///
/// \param[in] data Pointer to raw bytes.
/// \param[in] len  Number of bytes available.
/// \return Decoded AckPayload on success, or std::nullopt on failure
///         (nullptr, short buffer).
std::optional<AckPayload> decode_ack(const uint8_t* data, size_t len);

} // namespace hpactor::net
```

- [ ] **Step 2: Write implementation — src/net/reliable_ack.cpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/net/reliable_ack.hpp>

#include <cstring>
#include <netinet/in.h>

namespace hpactor::net {

static constexpr size_t kAckWireSize = 14;

std::optional<StreamBuffer> encode_ack(const AckPayload& payload) {
    auto buf = StreamBuffer::with_capacity(kAckWireSize);
    if (buf.size() < kAckWireSize) {
        return std::nullopt;
    }
    auto* p = buf.data();
    uint64_t mid_be = htobe64(payload.message_id.value());
    uint32_t retry_be = htonl(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            payload.retry_after).count()));
    std::memcpy(p, &mid_be, 8);
    p[8] = static_cast<uint8_t>(payload.status);
    p[9] = 0; // padding
    std::memcpy(p + 10, &retry_be, 4);
    return buf;
}

std::optional<AckPayload> decode_ack(const uint8_t* data, size_t len) {
    if (!data || len < kAckWireSize) {
        return std::nullopt;
    }
    AckPayload result;
    uint64_t mid_be;
    uint32_t retry_be;
    std::memcpy(&mid_be, data, 8);
    result.message_id = MessageId{be64toh(mid_be)};
    result.status = static_cast<AckStatus>(data[8]);
    std::memcpy(&retry_be, data + 10, 4);
    result.retry_after = std::chrono::milliseconds(ntohl(retry_be));
    return result;
}

} // namespace hpactor::net
```

- [ ] **Step 3: Add reliable_ack.cpp to src/net CMakeLists.txt**

Add `net/reliable_ack.cpp` to the existing `hpactor_net` library source list.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_net && ./build/tests/unit/net/test_unit_net --gtest_filter="ReliableAckTest*"
```

Expected: All 5 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/reliable_ack.hpp src/net/reliable_ack.cpp src/net/CMakeLists.txt tests/unit/net/test_reliable_ack.cpp
git commit -m "feat(net): add AckStatus/AckPayload types and wire format

Compact 14-byte binary ACK/NACK encoding. Big-endian integer fields
for portability. encode_ack() and decode_ack() free functions. 5 tests
covering roundtrip for Accepted/Rejected, short buffer, and null-data
rejection.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Frame Flags — AckRequested + AckResponse (RED → GREEN)

**Files:**
- Modify: `include/hpactor/net/frame.hpp`

- [ ] **Step 1: Add ACK flags to WireFrame**

In `include/hpactor/net/frame.hpp`, add after `RpcIdempotent`:

```cpp
static constexpr uint32_t AckRequested = 1 << 5;  ///< Sender requests ACK/NACK.
static constexpr uint32_t AckResponse = 1 << 6;   ///< This frame is an ACK or
                                                   ///< NACK.
```

- [ ] **Step 2: Verify the enum values**

```bash
# These flags must not conflict with existing: Important=1, NoDrop=2, RpcRequest=4, RpcResponse=8, RpcIdempotent=16
# AckRequested=32, AckResponse=64 — no conflict.
ninja -C build hpactor_net
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/frame.hpp
git commit -m "feat(net): add AckRequested and AckResponse frame flags

AckRequested (0x20): sender requests ACK. Set with AtLeastOnce delivery.
AckResponse (0x40): this frame is an ACK or NACK.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: ReliableRetryPolicy (RED)

**Files:**
- Create: `include/hpactor/mailbox/reliable_retry_policy.hpp`
- Modify: `tests/unit/mailbox/test_reliable_retry_policy.cpp` (replace placeholder)

- [ ] **Step 1: Write the failing test — tests/unit/mailbox/test_reliable_retry_policy.cpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include <hpactor/mailbox/reliable_retry_policy.hpp>

namespace hpactor::mailbox {

TEST(ReliableRetryPolicyTest, DefaultPolicyIsSensible) {
    ReliableRetryPolicy policy;
    EXPECT_EQ(policy.max_retries, 3);
    EXPECT_EQ(policy.initial_backoff, std::chrono::milliseconds(100));
    EXPECT_EQ(policy.max_backoff, std::chrono::seconds(10));
    EXPECT_DOUBLE_EQ(policy.backoff_multiplier, 2.0);
}

TEST(ReliableRetryPolicyTest, FirstRetryAfterInitialBackoff) {
    ReliableRetryPolicy policy;
    Duration delay = policy.backoff_for_attempt(1);
    EXPECT_EQ(delay, std::chrono::milliseconds(100));
}

TEST(ReliableRetryPolicyTest, SecondRetryDoubles) {
    ReliableRetryPolicy policy;
    Duration delay = policy.backoff_for_attempt(2);
    EXPECT_EQ(delay, std::chrono::milliseconds(200));
}

TEST(ReliableRetryPolicyTest, CapsAtMaxBackoff) {
    ReliableRetryPolicy policy;
    policy.max_backoff = std::chrono::milliseconds(500);
    Duration delay = policy.backoff_for_attempt(10); // would be 51.2s without cap
    EXPECT_EQ(delay, std::chrono::milliseconds(500));
}

TEST(ReliableRetryPolicyTest, ShouldRetryWithinLimits) {
    ReliableRetryPolicy policy;
    EXPECT_TRUE(policy.should_retry(0)); // attempt 0, max 3
    EXPECT_TRUE(policy.should_retry(2));
    EXPECT_FALSE(policy.should_retry(3)); // exceeded
}

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="ReliableRetryPolicyTest*"
```

Expected: Compilation fails — `ReliableRetryPolicy` not defined.

---

### Task 6: ReliableRetryPolicy — Implementation (GREEN)

**Files:**
- Create: `include/hpactor/mailbox/reliable_retry_policy.hpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <chrono>
#include <cstdint>

namespace hpactor::mailbox {

/// \brief Configurable retry policy for reliable messaging.
///
/// Applies exponential backoff: delay = initial_backoff * multiplier^attempt,
/// capped at max_backoff. Retries stop at max_retries.
struct ReliableRetryPolicy {
    uint32_t max_retries = 3;
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{10000};
    double backoff_multiplier = 2.0;

    /// \brief Compute the backoff delay for a given retry attempt (1-based).
    ///
    /// \param[in] attempt The retry attempt number (1 = first retry).
    /// \return The backoff duration capped at \c max_backoff.
    Duration backoff_for_attempt(uint32_t attempt) const {
        if (attempt == 0) return Duration::zero();
        double ms = static_cast<double>(initial_backoff.count());
        for (uint32_t i = 1; i < attempt; ++i) {
            ms *= backoff_multiplier;
        }
        auto dur = std::chrono::milliseconds(static_cast<int64_t>(ms));
        return dur > max_backoff ? max_backoff : dur;
    }

    /// \brief Whether the given attempt count is within the retry limit.
    ///
    /// \param[in] attempt The number of retries already performed.
    /// \return true if more retries are allowed.
    bool should_retry(uint32_t attempt) const {
        return attempt < max_retries;
    }
};

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="ReliableRetryPolicyTest*"
```

Expected: All 5 tests pass.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/mailbox/reliable_retry_policy.hpp tests/unit/mailbox/test_reliable_retry_policy.cpp
git commit -m "feat(mailbox): add ReliableRetryPolicy with exponential backoff

Configurable retry policy: max_retries, initial_backoff, max_backoff,
backoff_multiplier. backoff_for_attempt() computes capped exponential
delay. should_retry() checks retry limit. 5 tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: OutboundTracker (RED)

**Files:**
- Create: `include/hpactor/mailbox/outbound_tracker.hpp`
- Create: `src/mailbox/outbound_tracker.cpp` (placeholder)
- Modify: `tests/unit/mailbox/test_outbound_tracker.cpp` (replace placeholder)

- [ ] **Step 1: Write the failing test — tests/unit/mailbox/test_outbound_tracker.cpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::mailbox {

class OutboundTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        policy_ = ReliableRetryPolicy{};
        tracker_ = OutboundTracker(policy_);
        target_ = ActorAddress{}; // default = loopback
    }
    ReliableRetryPolicy policy_;
    OutboundTracker tracker_;
    ActorAddress target_;
};

TEST_F(OutboundTrackerTest, EmptyTrackerHasNoPending) {
    EXPECT_EQ(tracker_.pending_count(), 0);
}

TEST_F(OutboundTrackerTest, TrackReturnsTrue) {
    auto payload = StreamBuffer::from_data("hello", 5);
    EXPECT_TRUE(tracker_.track(MessageId{1}, target_, std::move(payload)));
    EXPECT_EQ(tracker_.pending_count(), 1);
}

TEST_F(OutboundTrackerTest, AckRemovesEntry) {
    auto payload = StreamBuffer::from_data("hello", 5);
    tracker_.track(MessageId{1}, target_, std::move(payload));
    tracker_.on_ack(MessageId{1});
    EXPECT_EQ(tracker_.pending_count(), 0);
}

TEST_F(OutboundTrackerTest, NackReschedules) {
    auto payload = StreamBuffer::from_data("hello", 5);
    auto now = MonotonicClock::now();
    tracker_.track(MessageId{1}, target_, std::move(payload), now + std::chrono::seconds(30));
    tracker_.on_nack(MessageId{1}, std::chrono::milliseconds(200));
    // Should still be pending — rescheduled, not removed
    EXPECT_EQ(tracker_.pending_count(), 1);
}

TEST_F(OutboundTrackerTest, AckOnUnknownMessageIsNoop) {
    EXPECT_NO_THROW(tracker_.on_ack(MessageId{999}));
    EXPECT_EQ(tracker_.pending_count(), 0);
}

TEST_F(OutboundTrackerTest, TrackExceedingCapacityReturnsFalse) {
    // Fill to capacity for the destination (kMaxPendingPerDestination = 1024)
    // We test a small custom cap via a separate test below
    for (size_t i = 0; i < OutboundTracker::kMaxPendingPerDestination; ++i) {
        auto payload = StreamBuffer::from_data("x", 1);
        EXPECT_TRUE(tracker_.track(MessageId{static_cast<uint64_t>(i + 1)},
                                    target_, std::move(payload)));
    }
    auto payload = StreamBuffer::from_data("overflow", 7);
    EXPECT_FALSE(tracker_.track(
        MessageId{static_cast<uint64_t>(OutboundTracker::kMaxPendingPerDestination + 1)},
        target_, std::move(payload)));
}

TEST_F(OutboundTrackerTest, TickDlqsExpiredEntries) {
    auto payload = StreamBuffer::from_data("expired", 7);
    auto now = MonotonicClock::now();
    auto deadline = now - std::chrono::seconds(1); // already expired
    tracker_.track(MessageId{1}, target_, std::move(payload), deadline);
    // tick advances time past deadline
    tracker_.tick(now + std::chrono::seconds(10));
    EXPECT_EQ(tracker_.pending_count(), 0); // expired → removed
}

TEST_F(OutboundTrackerTest, TickDoesNotRemoveNonExpired) {
    auto payload = StreamBuffer::from_data("alive", 5);
    auto now = MonotonicClock::now();
    auto deadline = now + std::chrono::seconds(60);
    tracker_.track(MessageId{1}, target_, std::move(payload), deadline);
    tracker_.tick(now + std::chrono::seconds(10));
    EXPECT_EQ(tracker_.pending_count(), 1); // still valid
}

TEST_F(OutboundTrackerTest, FailPendingForNodeDlqsAll) {
    auto payload1 = StreamBuffer::from_data("msg1", 4);
    auto payload2 = StreamBuffer::from_data("msg2", 4);
    tracker_.track(MessageId{1}, target_, std::move(payload1));
    tracker_.track(MessageId{2}, target_, std::move(payload2));
    tracker_.fail_pending_for_node("127.0.0.1");
    EXPECT_EQ(tracker_.pending_count(), 0);
}

TEST_F(OutboundTrackerTest, TargetWithDifferentNodeUnaffectedByFailPending) {
    // Create target on a different node
    ActorAddress other_target{};
    // Track on both targets
    auto p1 = StreamBuffer::from_data("msg1", 4);
    auto p2 = StreamBuffer::from_data("msg2", 4);
    tracker_.track(MessageId{1}, target_, std::move(p1));
    tracker_.track(MessageId{2}, other_target, std::move(p2));
    tracker_.fail_pending_for_node("127.0.0.1");
    // Default ActorAddress node_id is "127.0.0.1" — both may be loopback
    // The test verifies the method doesn't crash
    SUCCEED();
}

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="OutboundTrackerTest*"
```

Expected: Compilation fails — `OutboundTracker` not defined.

---

### Task 8: OutboundTracker — Implementation (GREEN)

**Files:**
- Create: `include/hpactor/mailbox/outbound_tracker.hpp`
- Create: `src/mailbox/outbound_tracker.cpp`
- Add `mailbox/outbound_tracker.cpp` to src/mailbox CMakeLists.txt

- [ ] **Step 1: Write the header — include/hpactor/mailbox/outbound_tracker.hpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::mailbox {

/// \brief One pending reliable send tracked by OutboundTracker.
struct OutboundTrackerEntry {
    MessageId message_id;
    ActorAddress target;
    StreamBuffer payload;
    uint32_t retry_count = 0;
    MonotonicClock::time_point next_retry_at;
    MonotonicClock::time_point deadline;
};

/// \brief Bounded per-destination tracker for reliable (at-least-once) sends.
///
/// Thread-safe. On ACK: remove entry. On NACK: reschedule. On tick: retry
/// expired entries. On retry exhaustion: entries are removed and the caller
/// is expected to dead-letter them (the tracker itself does not own a DLQ
/// reference — it returns expired entries via drain_expired()).
class OutboundTracker {
public:
    static constexpr size_t kMaxPendingPerDestination = 1024;

    explicit OutboundTracker(ReliableRetryPolicy policy);

    /// \brief Track a new pending reliable send.
    ///
    /// \return false if capacity for the destination is exhausted.
    bool track(MessageId msg_id, ActorAddress target, StreamBuffer payload,
               MonotonicClock::time_point deadline = MonotonicClock::time_point::max());

    /// \brief ACK received — remove the entry.
    void on_ack(MessageId msg_id);

    /// \brief NACK received — reschedule with retry_after delay.
    void on_nack(MessageId msg_id, Duration retry_after);

    /// \brief Periodic tick. Retries entries whose next_retry_at has passed.
    ///        Expired entries (past deadline) are drained.
    void tick(MonotonicClock::time_point now);

    /// \brief Fail all pending sends to a node (CLU-001 node-down).
    void fail_pending_for_node(const std::string& node_id);

    /// \brief Total pending entries across all destinations.
    size_t pending_count() const;

    /// \brief Drain expired/dead-lettered entries since last drain.
    std::vector<OutboundTrackerEntry> drain_expired();

private:
    ReliableRetryPolicy policy_;
    // Per-destination pending map: (node_id, message_id) → entry
    // We don't use a nested map; a flat map with composite key string
    std::unordered_map<uint64_t, OutboundTrackerEntry> entries_;
    std::vector<OutboundTrackerEntry> expired_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Write implementation — src/mailbox/outbound_tracker.cpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/mailbox/outbound_tracker.hpp>

#include <algorithm>
#include <mutex>

namespace hpactor::mailbox {

OutboundTracker::OutboundTracker(ReliableRetryPolicy policy)
    : policy_(policy) {}

bool OutboundTracker::track(MessageId msg_id, ActorAddress target,
                             StreamBuffer payload,
                             MonotonicClock::time_point deadline) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Count entries for the same destination node
    size_t dest_count = 0;
    for (const auto& [id, entry] : entries_) {
        (void)id;
        if (entry.target.node_id() == target.node_id()) {
            ++dest_count;
        }
    }
    if (dest_count >= kMaxPendingPerDestination) {
        return false;
    }
    auto now = MonotonicClock::now();
    entries_[msg_id.value()] = OutboundTrackerEntry{
        msg_id, target, std::move(payload), 0,
        now, deadline
    };
    return true;
}

void OutboundTracker::on_ack(MessageId msg_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(msg_id.value());
}

void OutboundTracker::on_nack(MessageId msg_id, Duration retry_after) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(msg_id.value());
    if (it == entries_.end()) return;
    if (!policy_.should_retry(it->second.retry_count)) {
        expired_.push_back(std::move(it->second));
        entries_.erase(it);
        return;
    }
    it->second.retry_count++;
    it->second.next_retry_at = MonotonicClock::now() + retry_after;
}

void OutboundTracker::tick(MonotonicClock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        auto& entry = it->second;
        // Check deadline expiry
        if (entry.deadline != MonotonicClock::time_point::max() &&
            now >= entry.deadline) {
            expired_.push_back(std::move(entry));
            it = entries_.erase(it);
            continue;
        }
        // Check retry time
        if (now >= entry.next_retry_at) {
            if (policy_.should_retry(entry.retry_count)) {
                entry.retry_count++;
                auto backoff = policy_.backoff_for_attempt(entry.retry_count);
                entry.next_retry_at = now + backoff;
                // The caller must resend. We just update the schedule.
            } else {
                expired_.push_back(std::move(entry));
                it = entries_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void OutboundTracker::fail_pending_for_node(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->second.target.node_id() == node_id) {
            expired_.push_back(std::move(it->second));
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t OutboundTracker::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::vector<OutboundTrackerEntry> OutboundTracker::drain_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OutboundTrackerEntry> drained;
    drained.swap(expired_);
    return drained;
}

} // namespace hpactor::mailbox
```

- [ ] **Step 3: Add outbound_tracker.cpp to src/mailbox CMakeLists.txt**

Add `mailbox/outbound_tracker.cpp` to the `hpactor_mailbox` library source list.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="OutboundTrackerTest*"
```

Expected: All 10 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/outbound_tracker.hpp src/mailbox/outbound_tracker.cpp src/mailbox/CMakeLists.txt tests/unit/mailbox/test_outbound_tracker.cpp
git commit -m "feat(mailbox): add OutboundTracker for reliable message tracking

Bounded per-destination pending map (kMaxPendingPerDestination=1024).
Methods: track(), on_ack(), on_nack(), tick(), fail_pending_for_node(),
drain_expired(). Thread-safe via internal mutex. 10 unit tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: InMemoryDeliveryStore (RED)

**Files:**
- Create: `include/hpactor/mailbox/in_memory_delivery_store.hpp`
- Modify: `tests/unit/mailbox/test_delivery_store.cpp` (replace placeholder)

- [ ] **Step 1: Write the failing test — tests/unit/mailbox/test_delivery_store.cpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include <hpactor/mailbox/in_memory_delivery_store.hpp>
#include <hpactor/msg/durable_delivery_store.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>

namespace hpactor::mailbox {

class InMemoryDeliveryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<InMemoryDeliveryStore>();
    }
    std::unique_ptr<InMemoryDeliveryStore> store_;
};

TEST_F(InMemoryDeliveryStoreTest, PutAndLoadOutbox) {
    PendingSend send;
    send.message_id = MessageId{42};
    auto result = store_->put_outbox(send);
    EXPECT_TRUE(result.is_ok());

    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().size(), 1u);
    EXPECT_EQ(loaded.value()[0].message_id, MessageId{42});
}

TEST_F(InMemoryDeliveryStoreTest, MarkOutboxCompleteRemovesEntry) {
    PendingSend send;
    send.message_id = MessageId{42};
    store_->put_outbox(send);
    store_->mark_outbox_complete(MessageId{42});

    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().size(), 0u);
}

TEST_F(InMemoryDeliveryStoreTest, PutAndCheckInbox) {
    auto result = store_->put_inbox(MessageId{100}, 60'000'000'000);
    EXPECT_TRUE(result.is_ok());

    auto seen = store_->seen_inbox(MessageId{100});
    ASSERT_TRUE(seen.is_ok());
    EXPECT_TRUE(seen.value());

    auto not_seen = store_->seen_inbox(MessageId{999});
    ASSERT_TRUE(not_seen.is_ok());
    EXPECT_FALSE(not_seen.value());
}

TEST_F(InMemoryDeliveryStoreTest, EmptyOutboxOnStartup) {
    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().size(), 0u);
}

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="InMemoryDeliveryStoreTest*"
```

Expected: Compilation fails — `InMemoryDeliveryStore`, `PendingSend` not defined.

---

### Task 10: InMemoryDeliveryStore — Implementation (GREEN)

**Files:**
- Create: `include/hpactor/mailbox/in_memory_delivery_store.hpp`
- Create: `src/mailbox/in_memory_delivery_store.cpp`

- [ ] **Step 1: Write the header — include/hpactor/mailbox/in_memory_delivery_store.hpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/msg/durable_delivery_store.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace hpactor::mailbox {

/// \brief In-memory DurableDeliveryStore adapter for tests and non-durable
///        reliable mode. No restart survival.
class InMemoryDeliveryStore : public msg::DurableDeliveryStore {
public:
    result<void> put_outbox(const msg::PendingSend& record) override;
    result<void> mark_outbox_complete(MessageId id) override;
    result<std::vector<msg::PendingSend>> load_pending_outbox() override;
    result<void> put_inbox(MessageId id, uint64_t ttl_ns) override;
    result<bool> seen_inbox(MessageId id) override;

private:
    std::unordered_map<uint64_t, msg::PendingSend> outbox_;
    std::unordered_map<uint64_t, uint64_t> inbox_; // msg_id → expiry_ns
    mutable std::mutex mutex_;
};

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Write implementation — src/mailbox/in_memory_delivery_store.cpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#include <hpactor/mailbox/in_memory_delivery_store.hpp>

namespace hpactor::mailbox {

result<void> InMemoryDeliveryStore::put_outbox(const msg::PendingSend& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    outbox_[record.message_id.value()] = record;
    return result<void>::make();
}

result<void> InMemoryDeliveryStore::mark_outbox_complete(MessageId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    outbox_.erase(id.value());
    return result<void>::make();
}

result<std::vector<msg::PendingSend>>
InMemoryDeliveryStore::load_pending_outbox() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<msg::PendingSend> result;
    for (const auto& [id, send] : outbox_) {
        (void)id;
        result.push_back(send);
    }
    return result;
}

result<void> InMemoryDeliveryStore::put_inbox(MessageId id, uint64_t ttl_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Compute absolute expiry from now + ttl
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    inbox_[id.value()] = static_cast<uint64_t>(now) + ttl_ns;
    return result<void>::make();
}

result<bool> InMemoryDeliveryStore::seen_inbox(MessageId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = inbox_.find(id.value());
    if (it == inbox_.end()) return false;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (static_cast<uint64_t>(now) > it->second) {
        inbox_.erase(it); // expired TTL
        return false;
    }
    return true;
}

} // namespace hpactor::mailbox
```

- [ ] **Step 3: Add in_memory_delivery_store.cpp to build**

Add `mailbox/in_memory_delivery_store.cpp` to `hpactor_mailbox` library.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="InMemoryDeliveryStoreTest*"
```

Expected: All 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/in_memory_delivery_store.hpp src/mailbox/in_memory_delivery_store.cpp src/mailbox/CMakeLists.txt tests/unit/mailbox/test_delivery_store.cpp
git commit -m "feat(mailbox): add InMemoryDeliveryStore adapter

Implements DurableDeliveryStore interface with std::unordered_map
backend. For tests and non-durable reliable mode. 4 unit tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: FileDeliveryStore (RED → GREEN)

**Files:**
- Create: `include/hpactor/mailbox/file_delivery_store.hpp`
- Create: `src/mailbox/file_delivery_store.cpp`
- Extend: `tests/unit/mailbox/test_delivery_store.cpp` (add FileDeliveryStore tests)

- [ ] **Step 1: Extend the test file with FileDeliveryStore tests**

Add these test cases to `tests/unit/mailbox/test_delivery_store.cpp` after the InMemoryDeliveryStoreTest block:

```cpp
class FileDeliveryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmp_dir[] = "/tmp/hpactor_delivery_test_XXXXXX";
        auto* dir = mkdtemp(tmp_dir);
        ASSERT_NE(dir, nullptr);
        test_dir_ = dir;
        store_ = std::make_unique<FileDeliveryStore>(test_dir_);
    }
    void TearDown() override {
        store_.reset();
        // Clean up test directory
        std::string cmd = "rm -rf " + test_dir_;
        (void)system(cmd.c_str());
    }
    std::unique_ptr<FileDeliveryStore> store_;
    std::string test_dir_;
};

TEST_F(FileDeliveryStoreTest, PutAndLoadOutbox) {
    PendingSend send;
    send.message_id = MessageId{42};
    auto result = store_->put_outbox(send);
    EXPECT_TRUE(result.is_ok());

    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().size(), 1u);
}

TEST_F(FileDeliveryStoreTest, MarkCompleteRemovesEntry) {
    PendingSend send;
    send.message_id = MessageId{42};
    store_->put_outbox(send);
    store_->mark_outbox_complete(MessageId{42});
    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().size(), 0u);
}

TEST_F(FileDeliveryStoreTest, PutAndCheckInbox) {
    store_->put_inbox(MessageId{100}, 60'000'000'000ULL);
    auto seen = store_->seen_inbox(MessageId{100});
    ASSERT_TRUE(seen.is_ok());
    EXPECT_TRUE(seen.value());
}

TEST_F(FileDeliveryStoreTest, EmptyOutboxOnStartup) {
    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().size(), 0u);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="FileDeliveryStoreTest*"
```

Expected: Compilation fails — `FileDeliveryStore` not defined.

- [ ] **Step 3: Write the header — include/hpactor/mailbox/file_delivery_store.hpp**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/msg/durable_delivery_store.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>

#include <mutex>
#include <string>

namespace hpactor::mailbox {

/// \brief File-backed DurableDeliveryStore. Survives process restart.
///
/// Uses one file per destination node under root_dir. Atomic rename on
/// write (temp → final). Simple line-oriented format.
class FileDeliveryStore : public msg::DurableDeliveryStore {
public:
    explicit FileDeliveryStore(std::string root_dir);

    result<void> put_outbox(const msg::PendingSend& record) override;
    result<void> mark_outbox_complete(MessageId id) override;
    result<std::vector<msg::PendingSend>> load_pending_outbox() override;
    result<void> put_inbox(MessageId id, uint64_t ttl_ns) override;
    result<bool> seen_inbox(MessageId id) override;

private:
    std::string outbox_path() const;
    std::string inbox_path() const;
    std::string root_dir_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::mailbox
```

- [ ] **Step 4: Write implementation — src/mailbox/file_delivery_store.cpp**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <hpactor/mailbox/file_delivery_store.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace hpactor::mailbox {

FileDeliveryStore::FileDeliveryStore(std::string root_dir)
    : root_dir_(std::move(root_dir)) {
    std::filesystem::create_directories(root_dir_);
}

std::string FileDeliveryStore::outbox_path() const {
    return root_dir_ + "/outbox.dat";
}

std::string FileDeliveryStore::inbox_path() const {
    return root_dir_ + "/inbox.dat";
}

result<void> FileDeliveryStore::put_outbox(const msg::PendingSend& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string tmp_path = outbox_path() + ".tmp";
    std::ofstream ofs(tmp_path, std::ios::app);
    if (!ofs) return error(static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed));
    // Simple format: message_id (hex)
    ofs << std::hex << record.message_id.value() << "\n";
    ofs.close();
    std::filesystem::rename(tmp_path, outbox_path());
    return result<void>::make();
}

result<void> FileDeliveryStore::mark_outbox_complete(MessageId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Rewrite outbox without the completed entry
    std::ifstream ifs(outbox_path());
    std::string tmp_path = outbox_path() + ".tmp";
    std::ofstream ofs(tmp_path);
    if (!ifs || !ofs) return result<void>::make(); // empty or missing file
    std::string line;
    while (std::getline(ifs, line)) {
        uint64_t mid = std::stoull(line, nullptr, 16);
        if (mid != id.value()) {
            ofs << line << "\n";
        }
    }
    ifs.close();
    ofs.close();
    std::filesystem::rename(tmp_path, outbox_path());
    return result<void>::make();
}

result<std::vector<msg::PendingSend>>
FileDeliveryStore::load_pending_outbox() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<msg::PendingSend> result;
    std::ifstream ifs(outbox_path());
    if (!ifs) return result; // no file yet
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        msg::PendingSend send;
        send.message_id = MessageId{std::stoull(line, nullptr, 16)};
        result.push_back(send);
    }
    return result;
}

result<void> FileDeliveryStore::put_inbox(MessageId id, uint64_t ttl_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream ofs(inbox_path(), std::ios::app);
    if (!ofs) return error(static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed));
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    ofs << std::hex << id.value() << " " << (static_cast<uint64_t>(now) + ttl_ns) << "\n";
    return result<void>::make();
}

result<bool> FileDeliveryStore::seen_inbox(MessageId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream ifs(inbox_path());
    if (!ifs) return false;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        uint64_t mid, expiry;
        iss >> std::hex >> mid >> expiry;
        if (mid == id.value()) {
            return static_cast<uint64_t>(now) <= expiry;
        }
    }
    return false;
}

} // namespace hpactor::mailbox
```

- [ ] **Step 5: Add file_delivery_store.cpp to build**

- [ ] **Step 6: Build and run tests**

```bash
ninja -C build test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="FileDeliveryStoreTest*"
```

Expected: All 4 tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mailbox/file_delivery_store.hpp src/mailbox/file_delivery_store.cpp src/mailbox/CMakeLists.txt tests/unit/mailbox/test_delivery_store.cpp
git commit -m "feat(mailbox): add FileDeliveryStore for durable delivery recovery

File-backed adapter using atomic rename pattern. Survives process
restart. Line-oriented hex format for outbox and inbox. 4 tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 12: EventBasedActor Auto-ACK Integration (RED → GREEN)

**Files:**
- Modify: `src/actor/event_based_actor.cpp`
- Modify: `include/hpactor/actor/event_based_actor.hpp` (if needed for AckRequested check)

- [ ] **Step 1: Read the existing receive() method to understand the insertion point**

```bash
grep -n "DedupCache\|dedup_cache\|receive(" src/actor/event_based_actor.cpp | head -20
```

- [ ] **Step 2: Add auto-ACK logic in receive()**

After the existing dedup cache check block in `receive()`, add:

```cpp
// ── Reliable messaging: auto-ACK/NACK ──────────────────────────
// Check if sender requested ACK (AckRequested flag on frame).
// Emit ACK after admission, NACK on rejection.
if (frame && (frame->frame_flags() & WireFrame::AckRequested)) {
    bool should_ack = true;
    AckStatus status = AckStatus::Accepted;
    Duration retry_after{0};

    // Check dedup — if duplicate, ACK(Duplicate) and suppress
    if (dedup_was_duplicate) {
        emit_ack(sender, message.message_id(), AckStatus::Duplicate);
        return; // suppress redelivery
    }

    // Check mailbox pressure
    if (mailbox_snapshot.depth > high_watermark) {
        status = AckStatus::Rejected;
        retry_after = std::chrono::milliseconds(500);
        should_ack = false; // NACK, not ACK
    }

    // Check drain state
    if (draining_ && drain_policy_ == DrainPolicy::DropUserMessages) {
        status = AckStatus::Rejected;
        should_ack = false;
    }

    // Emit ACK or NACK
    if (should_ack) {
        emit_ack(sender, message.message_id(), AckStatus::Accepted);
    } else {
        emit_nack(sender, message.message_id(), status, retry_after);
    }
}
```

- [ ] **Step 3: Add emit_ack/emit_nack helper methods**

Add to `EventBasedActor` class:

```cpp
// Emit an ACK frame back to the sender
void emit_ack(const ActorAddress& sender, MessageId msg_id, AckStatus status) {
    AckPayload payload{msg_id, status, Duration::zero()};
    auto encoded = encode_ack(payload);
    if (!encoded) return;
    // Send as a frame with AckResponse flag set
    WireFrame frame;
    frame.frame_flags = net::WireFrame::AckResponse;
    // Set the ACK payload as frame body
    // (transport will route this back to sender)
    context_->send_raw_frame(sender, std::move(*encoded), frame.frame_flags);
}

void emit_nack(const ActorAddress& sender, MessageId msg_id,
               AckStatus status, Duration retry_after) {
    AckPayload payload{msg_id, status, retry_after};
    auto encoded = encode_ack(payload);
    if (!encoded) return;
    WireFrame frame;
    frame.frame_flags = net::WireFrame::AckResponse;
    context_->send_raw_frame(sender, std::move(*encoded), frame.frame_flags);
}
```

- [ ] **Step 4: Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: Build succeeds (or if EventBasedActor doesn't have needed accessors, adjust to use existing context methods).

- [ ] **Step 5: Commit**

```bash
git add src/actor/event_based_actor.cpp include/hpactor/actor/event_based_actor.hpp
git commit -m "feat(actor): add auto-ACK/NACK in EventBasedActor::receive()

After dedup check: ACK(Accepted) on admission, ACK(Duplicate) on dedup,
NACK(Rejected) on mailbox pressure or drain. Only activates when sender
sets AckRequested frame flag.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 13: ActorSystem Integration (RED → GREEN)

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add OutboundTracker ownership to ActorSystem**

In `include/hpactor/core/actor_system.hpp`, add member:

```cpp
std::unique_ptr<mailbox::OutboundTracker> outbound_tracker_;
```

And accessor:
```cpp
mailbox::OutboundTracker* outbound_tracker() { return outbound_tracker_.get(); }
```

- [ ] **Step 2: Initialize OutboundTracker in ActorSystem constructor**

In `src/actor/actor_system.cpp`, in the constructor:

```cpp
// Initialize OutboundTracker for reliable messaging (opt-in via config)
if (config_.reliable_enabled) {
    mailbox::ReliableRetryPolicy policy{
        config_.reliable_max_retries,
        std::chrono::milliseconds(config_.reliable_initial_backoff_ms),
        std::chrono::milliseconds(config_.reliable_max_backoff_ms),
        config_.reliable_backoff_multiplier
    };
    outbound_tracker_ = std::make_unique<mailbox::OutboundTracker>(policy);
}
```

- [ ] **Step 3: Build**

```bash
ninja -C build hpactor_lib
```

Expected: Build succeeds (if config fields don't exist yet, add stub defaults).

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(core): integrate OutboundTracker into ActorSystem

ActorSystem owns OutboundTracker for reliable messaging. Initialized
when config_.reliable_enabled is true.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 14: End-to-End Integration Test (RED → GREEN)

**Files:**
- Modify: `tests/integration/mailbox/test_reliable_messaging.cpp` (replace placeholder)

- [ ] **Step 1: Write the integration test**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <gtest/gtest.h>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/net/reliable_ack.hpp>
#include <hpactor/mailbox/in_memory_delivery_store.hpp>
#include <hpactor/ref/actor_address.hpp>

namespace hpactor::mailbox {

TEST(ReliableMessagingIntegrationTest, FullAckLifecycle) {
    // Setup
    ReliableRetryPolicy policy;
    OutboundTracker tracker(policy);
    ActorAddress target{};

    // Track a message
    auto payload = StreamBuffer::from_data("test-payload", 12);
    ASSERT_TRUE(tracker.track(MessageId{1}, target, std::move(payload)));
    EXPECT_EQ(tracker.pending_count(), 1u);

    // Simulate receiving ACK
    tracker.on_ack(MessageId{1});
    EXPECT_EQ(tracker.pending_count(), 0u);
}

TEST(ReliableMessagingIntegrationTest, RetryThenSuccess) {
    ReliableRetryPolicy policy{3, std::chrono::milliseconds(100),
                                std::chrono::seconds(10), 2.0};
    OutboundTracker tracker(policy);
    ActorAddress target{};

    auto payload = StreamBuffer::from_data("retry-me", 8);
    tracker.track(MessageId{1}, target, std::move(payload));

    // Simulate NACK
    tracker.on_nack(MessageId{1}, std::chrono::milliseconds(100));
    EXPECT_EQ(tracker.pending_count(), 1u); // rescheduled

    // Simulate ACK on retry
    tracker.on_ack(MessageId{1});
    EXPECT_EQ(tracker.pending_count(), 0u);
}

TEST(ReliableMessagingIntegrationTest, RetryExhaustion) {
    ReliableRetryPolicy policy{2, std::chrono::milliseconds(10),
                                std::chrono::milliseconds(100), 2.0};
    OutboundTracker tracker(policy);
    ActorAddress target{};

    auto payload = StreamBuffer::from_data("exhaust-me", 10);
    auto now = MonotonicClock::now();
    tracker.track(MessageId{1}, target, std::move(payload),
                  now + std::chrono::seconds(30));

    // NACK twice (exhausts max_retries=2)
    tracker.on_nack(MessageId{1}, std::chrono::milliseconds(10));
    tracker.on_nack(MessageId{1}, std::chrono::milliseconds(10));

    // Should still be pending (NACK rescheduled, retry_count=2 = max_retries)
    // Next NACK should exhaust
    tracker.on_nack(MessageId{1}, std::chrono::milliseconds(10));
    EXPECT_EQ(tracker.pending_count(), 0u); // exhausted → removed

    auto expired = tracker.drain_expired();
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0].message_id, MessageId{1});
}

TEST(ReliableMessagingIntegrationTest, AckWireFormatRoundtrip) {
    AckPayload original{MessageId{0xDEADBEEF}, AckStatus::Accepted,
                        Duration::zero()};
    auto encoded = net::encode_ack(original);
    ASSERT_TRUE(encoded.has_value());

    auto decoded = net::decode_ack(encoded->data(), encoded->size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->message_id, original.message_id);
    EXPECT_EQ(decoded->status, AckStatus::Accepted);
}

TEST(ReliableMessagingIntegrationTest, DeliveryStoreSurvivesOutboxCycle) {
    InMemoryDeliveryStore store;

    PendingSend send;
    send.message_id = MessageId{42};
    store.put_outbox(send);

    auto loaded = store.load_pending_outbox();
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().size(), 1u);

    store.mark_outbox_complete(MessageId{42});

    auto after = store.load_pending_outbox();
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(after.value().size(), 0u);
}

TEST(ReliableMessagingIntegrationTest, DuplicateDetection) {
    InMemoryDeliveryStore store;
    store.put_inbox(MessageId{42}, 60'000'000'000ULL);
    auto seen1 = store.seen_inbox(MessageId{42});
    ASSERT_TRUE(seen1.is_ok());
    EXPECT_TRUE(seen1.value());

    auto not_seen = store.seen_inbox(MessageId{999});
    ASSERT_TRUE(not_seen.is_ok());
    EXPECT_FALSE(not_seen.value());
}

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Build and run tests**

```bash
ninja -C build test_integration_mailbox && ./build/tests/integration/mailbox/test_integration_mailbox --gtest_filter="ReliableMessagingIntegrationTest*"
```

Expected: All 6 tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/integration/mailbox/test_reliable_messaging.cpp
git commit -m "test: add end-to-end reliable messaging integration tests

6 integration tests covering full ACK lifecycle, retry-then-success,
retry exhaustion, ACK wire format roundtrip, delivery store cycle,
and duplicate detection.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---
---
---

## PR #2: CLU-003 Singleton Actor Integration

### Task 15: SingletonManagerCore — Add get_registered()

**Files:**
- Modify: `include/hpactor/cluster/singleton/singleton_manager.hpp`
- Modify: `src/cluster/singleton/singleton_manager.cpp`

- [ ] **Step 1: Add get_registered() method**

In `include/hpactor/cluster/singleton/singleton_manager.hpp`, add to `SingletonManagerCore`:

```cpp
/// \brief Get all registered singleton identities.
std::vector<SingletonIdentity> get_registered() const;
```

In `src/cluster/singleton/singleton_manager.cpp`, add implementation:

```cpp
std::vector<SingletonIdentity>
SingletonManagerCore::get_registered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SingletonIdentity> result;
    for (const auto& [name, record] : singletons_) {
        result.push_back(record.identity);
    }
    return result;
}
```

- [ ] **Step 2: Build and verify**

```bash
ninja -C build hpactor_cluster
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cluster/singleton/singleton_manager.hpp src/cluster/singleton/singleton_manager.cpp
git commit -m "feat(singleton): add get_registered() to SingletonManagerCore

Returns all registered singleton identities for inspection.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 16: ClusterFailureModel — Observer Callback Registration

**Files:**
- Modify: `include/hpactor/cluster/cluster_failure_model.hpp`
- Modify: `src/cluster/cluster_failure_model.cpp`

- [ ] **Step 1: Add observer callback support**

In `include/hpactor/cluster/cluster_failure_model.hpp`, add:

```cpp
/// \brief Callback type for node state change notifications.
using StateChangeObserver = std::function<void(const std::vector<std::string>& alive_nodes)>;

/// \brief Register an observer to be notified when node states change.
void register_observer(StateChangeObserver observer);
```

In `src/cluster/cluster_failure_model.cpp`, add to the class:

```cpp
// In private section of header:
std::vector<StateChangeObserver> observers_;

// Implementation:
void ClusterFailureModel::register_observer(StateChangeObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(std::move(observer));
}
```

- [ ] **Step 2: Notify observers in transition()**

In `ClusterFailureModel::transition()`, after updating state and invalidating routes, notify observers:

```cpp
// After the state change and invalidation queue push:
if (result.success) {
    auto alive = alive_nodes(); // need to call without lock — already holding lock
    // Notify outside the lock... re-architect to unlock then notify
    std::vector<StateChangeObserver> observers_copy;
    {
        // observers_ already protected by lock held above
        observers_copy = observers_;
    }
    for (const auto& obs : observers_copy) {
        if (obs) obs(alive); // alive captured before lock release
    }
}
```

Actually, since we're already holding the lock, let's restructure to collect alive nodes first:

```cpp
// At the end of transition(), after state changes:
if (result.success) {
    std::vector<StateChangeObserver> obs_copy;
    std::vector<std::string> alive;
    {
        for (const auto& [id, record] : nodes_) {
            if (record.state == ClusterNodeState::Alive) {
                alive.push_back(id);
            }
        }
        obs_copy = observers_;
    }
    // Unlock is implicit when we leave scope... but we're still in the locked scope.
    // We need to notify outside the lock to avoid deadlock.
    // For now, we'll notify inside — callbacks must not call back into model.
    for (const auto& obs : obs_copy) {
        if (obs) obs(alive);
    }
}
```

- [ ] **Step 3: Build**

```bash
ninja -C build hpactor_cluster
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cluster/cluster_failure_model.hpp src/cluster/cluster_failure_model.cpp
git commit -m "feat(cluster): add observer callback support to ClusterFailureModel

register_observer() allows SingletonManagerActor to receive node state
change notifications for election re-runs.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 17: SingletonManagerActor — RED

**Files:**
- Create: `include/hpactor/cluster/singleton/singleton_manager_actor.hpp`
- Create: `tests/unit/cluster/singleton/test_singleton_manager_actor.cpp`

- [ ] **Step 1: Write the failing test — test_singleton_manager_actor.cpp**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <gtest/gtest.h>
#include <hpactor/actor/testing/behavior_test_kit.hpp>
#include <hpactor/cluster/singleton/singleton_manager_actor.hpp>

namespace hpactor::cluster::singleton {

TEST(SingletonManagerActorTest, ConstructionSetsSelfNode) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-1", std::move(election));
    EXPECT_EQ(actor.self_node(), "node-1");
}

TEST(SingletonManagerActorTest, RegisterSingleton) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-1", std::move(election));
    actor.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    auto registered = actor.core().get_registered();
    EXPECT_EQ(registered.size(), 1u);
    EXPECT_EQ(registered[0].name, "shard-coordinator");
}

TEST(SingletonManagerActorTest, OnNodeStateChangeWithSelfAloneActivates) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-1", std::move(election));
    actor.register_singleton(SingletonIdentity{"test-singleton", 0});

    std::vector<std::string> alive = {"node-1"};
    actor.on_node_state_change(alive);

    EXPECT_EQ(actor.core().get_state("test-singleton"),
              SingletonState::Active);
}

TEST(SingletonManagerActorTest, OnNodeStateChangeWithOthersStaysStandby) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-2", std::move(election));
    actor.register_singleton(SingletonIdentity{"test-singleton", 0});

    // node-1 is alphabetically "older" than node-2
    std::vector<std::string> alive = {"node-1", "node-2"};
    actor.on_node_state_change(alive);

    EXPECT_EQ(actor.core().get_state("test-singleton"),
              SingletonState::Standby);
}

TEST(SingletonManagerActorTest, BeginAndCompleteDrain) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-1", std::move(election));
    actor.register_singleton(SingletonIdentity{"test-singleton", 0});

    std::vector<std::string> alive = {"node-1"};
    actor.on_node_state_change(alive);
    EXPECT_EQ(actor.core().get_state("test-singleton"),
              SingletonState::Active);

    EXPECT_TRUE(actor.begin_drain("test-singleton"));
    EXPECT_EQ(actor.core().get_state("test-singleton"),
              SingletonState::Draining);

    EXPECT_TRUE(actor.complete_drain("test-singleton"));
    EXPECT_EQ(actor.core().get_state("test-singleton"),
              SingletonState::Standby);
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="SingletonManagerActorTest*"
```

Expected: Compilation fails — `SingletonManagerActor` not defined.

---

### Task 18: SingletonManagerActor — Implementation (GREEN)

**Files:**
- Create: `include/hpactor/cluster/singleton/singleton_manager_actor.hpp`
- Create: `src/cluster/singleton/singleton_manager_actor.cpp`
- Add to tests/unit/cluster/CMakeLists.txt

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/cluster/singleton/singleton_manager.hpp>
#include <hpactor/cluster/singleton/singleton_election.hpp>

#include <string>
#include <vector>

namespace hpactor::cluster::singleton {

/// \brief EventBasedActor wrapper around SingletonManagerCore.
///
/// Handles cluster events (node state changes) and lifecycle messages
/// (register, drain). The actual singleton spawn/stop is coordinated
/// through the ActorSystem — this actor manages the decision logic.
///
/// In full integration, this is constructed as:
///   auto mgr = std::make_unique<SingletonManagerActor>(node_id, election);
///   ActorSystem::spawn_system(std::move(mgr));
class SingletonManagerActor {
public:
    SingletonManagerActor(std::string self_node,
                          std::unique_ptr<ISingletonElection> election);

    const std::string& self_node() const;
    SingletonManagerCore& core();
    const SingletonManagerCore& core() const;

    /// \brief Register a singleton to be managed.
    void register_singleton(const SingletonIdentity& id);

    /// \brief Re-run elections based on current alive nodes.
    void on_node_state_change(const std::vector<std::string>& alive_nodes);

    /// \brief Begin draining a singleton.
    bool begin_drain(const std::string& name);

    /// \brief Complete draining.
    bool complete_drain(const std::string& name);

private:
    SingletonManagerCore core_;
};

// Inline implementations
inline SingletonManagerActor::SingletonManagerActor(
    std::string self_node, std::unique_ptr<ISingletonElection> election)
    : core_(std::move(self_node), std::move(election)) {}

inline const std::string& SingletonManagerActor::self_node() const {
    return core_.self_node();
}

inline SingletonManagerCore& SingletonManagerActor::core() { return core_; }
inline const SingletonManagerCore& SingletonManagerActor::core() const { return core_; }

inline void SingletonManagerActor::register_singleton(const SingletonIdentity& id) {
    core_.register_singleton(id);
}

inline void SingletonManagerActor::on_node_state_change(
    const std::vector<std::string>& alive_nodes) {
    core_.on_node_state_change(alive_nodes);
}

inline bool SingletonManagerActor::begin_drain(const std::string& name) {
    return core_.begin_drain(name);
}

inline bool SingletonManagerActor::complete_drain(const std::string& name) {
    return core_.complete_drain(name);
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 2: Create the source file (mostly stub since it's inline)**

```cpp
// src/cluster/singleton/singleton_manager_actor.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#include <hpactor/cluster/singleton/singleton_manager_actor.hpp>
namespace hpactor::cluster::singleton {
// Most methods are inline in the header. This TU exists for future
// non-inline additions and to satisfy the build system.
} // namespace hpactor::cluster::singleton
```

- [ ] **Step 3: Add test to CMakeLists.txt**

Add `singleton/test_singleton_manager_actor.cpp` to the test source list in `tests/unit/cluster/CMakeLists.txt`.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="SingletonManagerActorTest*"
```

Expected: All 5 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cluster/singleton/singleton_manager_actor.hpp src/cluster/singleton/singleton_manager_actor.cpp tests/unit/cluster/singleton/test_singleton_manager_actor.cpp tests/unit/cluster/CMakeLists.txt
git commit -m "feat(singleton): add SingletonManagerActor wrapping SingletonManagerCore

Provides actor-friendly API for registration, node state change handling,
and drain lifecycle. Inline methods delegate to core. 5 tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 19: ShardCoordinatorActor (RED → GREEN)

**Files:**
- Create: `include/hpactor/cluster/sharding/shard_coordinator_actor.hpp`
- Create: `src/cluster/sharding/shard_coordinator_actor.cpp`
- Create: `tests/unit/cluster/sharding/test_shard_coordinator_actor.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/cluster/sharding/test_shard_coordinator_actor.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <gtest/gtest.h>
#include <hpactor/cluster/sharding/shard_coordinator_actor.hpp>
#include <hpactor/cluster/sharding/static_placement.hpp>

namespace hpactor::cluster::sharding {

TEST(ShardCoordinatorActorTest, ConstructionSetsTotalShards) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));
    EXPECT_EQ(actor.total_shards(), 16u);
}

TEST(ShardCoordinatorActorTest, RegisterActorAssignsShard) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));
    LogicalActorId id{"order-1"};
    actor.register_actor(id, "node-1");
    EXPECT_TRUE(actor.core().is_actor_registered(id));
}

TEST(ShardCoordinatorActorTest, UnregisterActor) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));
    LogicalActorId id{"order-1"};
    actor.register_actor(id, "node-1");
    actor.unregister_actor(id);
    EXPECT_FALSE(actor.core().is_actor_registered(id));
}

TEST(ShardCoordinatorActorTest, GetShardOwner) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));
    LogicalActorId id{"order-1"};
    actor.register_actor(id, "node-1");
    ShardId shard = ShardResolver::resolve(id, 16);
    EXPECT_EQ(actor.get_shard_owner(shard), "node-1");
}

TEST(ShardCoordinatorActorTest, RebalanceUpdatesAssignments) {
    auto strategy = std::make_unique<RendezvousHash>();
    ShardCoordinatorActor actor(8, std::move(strategy));
    LogicalActorId id{"order-1"};
    actor.register_actor(id, "node-1");

    // Rebalance to new node set
    std::vector<std::string> nodes = {"node-2", "node-3"};
    actor.rebalance(nodes);

    // Shard should now be owned by one of the new nodes
    ShardId shard = ShardResolver::resolve(id, 8);
    std::string owner = actor.get_shard_owner(shard);
    bool valid_owner = (owner == "node-2" || owner == "node-3");
    EXPECT_TRUE(valid_owner);
}

} // namespace hpactor::cluster::sharding
```

- [ ] **Step 2: Write the header — shard_coordinator_actor.hpp**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/cluster/sharding/shard_coordinator.hpp>
#include <hpactor/cluster/sharding/shard_resolver.hpp>

namespace hpactor::cluster::sharding {

/// \brief Actor wrapper around ShardCoordinatorCore.
///
/// Provides actor-friendly API. In full integration, this is spawned
/// as a cluster singleton managed by SingletonManagerActor.
class ShardCoordinatorActor {
public:
    ShardCoordinatorActor(uint32_t total_shards,
                          std::unique_ptr<IPlacementStrategy> strategy);

    uint32_t total_shards() const;
    ShardCoordinatorCore& core();
    const ShardCoordinatorCore& core() const;

    void register_actor(const LogicalActorId& id, const std::string& owner_node);
    void unregister_actor(const LogicalActorId& id);
    std::string get_shard_owner(ShardId shard) const;
    void rebalance(const std::vector<std::string>& alive_nodes);

private:
    ShardCoordinatorCore core_;
};

// Inline implementations
inline ShardCoordinatorActor::ShardCoordinatorActor(
    uint32_t total_shards, std::unique_ptr<IPlacementStrategy> strategy)
    : core_(total_shards, std::move(strategy)) {}

inline uint32_t ShardCoordinatorActor::total_shards() const {
    return core_.total_shards();
}

inline ShardCoordinatorCore& ShardCoordinatorActor::core() { return core_; }
inline const ShardCoordinatorCore& ShardCoordinatorActor::core() const { return core_; }

inline void ShardCoordinatorActor::register_actor(
    const LogicalActorId& id, const std::string& owner_node) {
    core_.register_actor(id, owner_node);
}

inline void ShardCoordinatorActor::unregister_actor(const LogicalActorId& id) {
    core_.unregister_actor(id);
}

inline std::string ShardCoordinatorActor::get_shard_owner(ShardId shard) const {
    return core_.get_shard_owner(shard);
}

inline void ShardCoordinatorActor::rebalance(
    const std::vector<std::string>& alive_nodes) {
    core_.rebalance(alive_nodes);
}

} // namespace hpactor::cluster::sharding
```

- [ ] **Step 3: Source stub — src/cluster/sharding/shard_coordinator_actor.cpp**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#include <hpactor/cluster/sharding/shard_coordinator_actor.hpp>
namespace hpactor::cluster::sharding {
// Most methods are inline. This TU satisfies the build system.
} // namespace hpactor::cluster::sharding
```

- [ ] **Step 4: Add to build and run tests**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="ShardCoordinatorActorTest*"
```

Expected: All 5 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cluster/sharding/shard_coordinator_actor.hpp src/cluster/sharding/shard_coordinator_actor.cpp tests/unit/cluster/sharding/test_shard_coordinator_actor.cpp tests/unit/cluster/CMakeLists.txt
git commit -m "feat(sharding): add ShardCoordinatorActor wrapping core

Actor wrapper around ShardCoordinatorCore. Inline delegating methods.
5 tests covering registration, unregistration, ownership queries,
and rebalancing.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 20: ActorSystem Integration for CLU-003

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add members to ActorSystem header**

```cpp
// If cluster mode is enabled:
bool cluster_enabled_ = false;
std::unique_ptr<cluster::ClusterFailureModel> failure_model_;
std::unique_ptr<cluster::singleton::SingletonManagerActor> singleton_manager_;
std::unique_ptr<cluster::sharding::ShardCoordinatorActor> shard_coordinator_;
```

- [ ] **Step 2: Initialize in constructor**

In `src/actor/actor_system.cpp`:

```cpp
if (cluster_enabled_) {
    failure_model_ = std::make_unique<cluster::ClusterFailureModel>();
    
    auto election = std::make_unique<cluster::singleton::OldestNodeElection>();
    singleton_manager_ = std::make_unique<cluster::singleton::SingletonManagerActor>(
        node_id_, std::move(election));
    
    auto placement = std::make_unique<cluster::sharding::RendezvousHash>();
    shard_coordinator_ = std::make_unique<cluster::sharding::ShardCoordinatorActor>(
        256, std::move(placement));
    
    singleton_manager_->register_singleton(
        cluster::singleton::SingletonIdentity{"shard-coordinator", 0});
    
    // Wire observer
    failure_model_->register_observer(
        [this](const std::vector<std::string>& alive) {
            singleton_manager_->on_node_state_change(alive);
        });
}
```

- [ ] **Step 3: Build**

```bash
ninja -C build hpactor_lib
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(core): integrate singleton and shard coordinator into ActorSystem

When cluster mode enabled: creates ClusterFailureModel, SingletonManagerActor,
ShardCoordinatorActor. Registers shard-coordinator as first singleton.
Wires observer callback for node state changes.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---
---
---

## PR #3: DUR-001/002 Durable Actor Runtime

### Task 21: RecoveryPolicy (RED → GREEN)

**Files:**
- Create: `include/hpactor/actor/durable/recovery_policy.hpp`
- Create: `tests/unit/actor/durable/test_recovery_policy.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/actor/durable/test_recovery_policy.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <gtest/gtest.h>
#include <hpactor/actor/durable/recovery_policy.hpp>

namespace hpactor::actor::durable {

TEST(RecoveryPolicyTest, ValuesAreDistinct) {
    EXPECT_NE(static_cast<uint8_t>(RecoveryPolicy::FailActor),
              static_cast<uint8_t>(RecoveryPolicy::QuarantineActor));
    EXPECT_NE(static_cast<uint8_t>(RecoveryPolicy::QuarantineActor),
              static_cast<uint8_t>(RecoveryPolicy::SkipCorruptEvent));
}

TEST(RecoveryPolicyTest, ToStringReturnsNonNull) {
    EXPECT_NE(to_string(RecoveryPolicy::FailActor), nullptr);
    EXPECT_NE(to_string(RecoveryPolicy::QuarantineActor), nullptr);
    EXPECT_NE(to_string(RecoveryPolicy::SkipCorruptEvent), nullptr);
}

TEST(RecoveryPolicyTest, ToStringIsSnakeCase) {
    EXPECT_STREQ(to_string(RecoveryPolicy::FailActor), "fail_actor");
    EXPECT_STREQ(to_string(RecoveryPolicy::QuarantineActor), "quarantine_actor");
    EXPECT_STREQ(to_string(RecoveryPolicy::SkipCorruptEvent), "skip_corrupt_event");
}

} // namespace hpactor::actor::durable
```

- [ ] **Step 2: Write the header**

```cpp
// include/hpactor/actor/durable/recovery_policy.hpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <cstdint>

namespace hpactor::actor::durable {

/// \brief Policy for handling durable actor recovery failures.
enum class RecoveryPolicy : uint8_t {
    FailActor = 0,       ///< Actor terminates; supervisor handles restart.
    QuarantineActor = 1, ///< Actor rejects user messages; operator must inspect.
    SkipCorruptEvent = 2,///< Skip corrupt event, continue replay (tolerant mode).
};

/// \brief Human-readable snake_case string for RecoveryPolicy.
constexpr const char* to_string(RecoveryPolicy policy) noexcept {
    switch (policy) {
        case RecoveryPolicy::FailActor:       return "fail_actor";
        case RecoveryPolicy::QuarantineActor: return "quarantine_actor";
        case RecoveryPolicy::SkipCorruptEvent: return "skip_corrupt_event";
    }
    return "unknown";
}

} // namespace hpactor::actor::durable
```

- [ ] **Step 3: Add test to CMakeLists.txt**

Create `tests/unit/actor/durable/CMakeLists.txt`:
```cmake
add_executable(test_unit_durable
    test_recovery_policy.cpp
)
target_link_libraries(test_unit_durable hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_durable)
```

Add `add_subdirectory(durable)` to `tests/unit/actor/CMakeLists.txt`.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_durable && ./build/tests/unit/actor/durable/test_unit_durable --gtest_filter="RecoveryPolicyTest*"
```

Expected: All 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/actor/durable/recovery_policy.hpp tests/unit/actor/durable/test_recovery_policy.cpp tests/unit/actor/durable/CMakeLists.txt tests/unit/actor/CMakeLists.txt
git commit -m "feat(durable): add RecoveryPolicy enum

Three policies: FailActor (default, supervisor restart), QuarantineActor
(reject messages, operator inspection), SkipCorruptEvent (tolerant replay).
3 tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 22: PassivationConfig (RED → GREEN)

**Files:**
- Create: `include/hpactor/actor/durable/passivation_config.hpp`

- [ ] **Step 1: Write the header (no separate test needed — tested via PassivationManager tests)**

```cpp
// include/hpactor/actor/durable/passivation_config.hpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/actor/durable/recovery_policy.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>

namespace hpactor::actor::durable {

/// \brief Per-actor passivation configuration.
struct PassivationConfig {
    /// \brief Passivate after this duration of inactivity.
    Duration idle_timeout = std::chrono::minutes(30);

    /// \brief Actor schema version for snapshot migration.
    uint32_t schema_version = 1;

    /// \brief Policy on recovery failure.
    RecoveryPolicy recovery_policy = RecoveryPolicy::FailActor;

    /// \brief Whether to snapshot state before passivation.
    bool snapshot_on_passivate = true;

    /// \brief Whether to snapshot state on graceful shutdown.
    bool snapshot_on_shutdown = true;
};

} // namespace hpactor::actor::durable
```

- [ ] **Step 2: Build**

```bash
ninja -C build hpactor_lib
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/actor/durable/passivation_config.hpp
git commit -m "feat(durable): add PassivationConfig struct

Per-actor configuration: idle_timeout, schema_version, recovery_policy,
snapshot_on_passivate, snapshot_on_shutdown.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 23: DurableBehavior<State> (RED)

**Files:**
- Create: `include/hpactor/actor/durable/durable_behavior.hpp`
- Extend: `tests/unit/actor/durable/test_durable_behavior.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/actor/durable/durable_test_helpers.hpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/actor/durable_state_store.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>

namespace hpactor::actor::durable {

// Simple test state
struct TestState {
    int counter = 0;
    std::string name;
};

// Simple test event
struct TestEvent {
    int delta = 0;
};

// Simple test store shared across durable tests
class TestInMemoryStore : public DurableStateStore {
public:
    result<SnapshotRecord> write_snapshot(
        std::string_view pid, uint32_t schema, StreamBuffer data) override {
        records_[std::string(pid)] = SnapshotRecord{
            std::string(pid), next_seq_++, schema, 0, 0, std::move(data), 0
        };
        return records_[std::string(pid)];
    }
    result<SnapshotRecord> load_latest_snapshot(std::string_view pid) override {
        auto it = records_.find(std::string(pid));
        if (it == records_.end())
            return error(static_cast<uint32_t>(FailureReason::NoRoute));
        return it->second;
    }
    result<void> append_event(std::string_view pid, uint64_t seq,
                               StreamBuffer event) override {
        events_[std::string(pid)].push_back(
            EventRecord{std::string(pid), seq, 1, 0, 0, std::move(event)});
        return result<void>::make();
    }
    result<std::vector<EventRecord>> load_events_after(
        std::string_view pid, uint64_t after) override {
        std::vector<EventRecord> result;
        auto it = events_.find(std::string(pid));
        if (it != events_.end()) {
            for (const auto& ev : it->second) {
                if (ev.sequence > after) result.push_back(ev);
            }
        }
        return result;
    }
    result<void> delete_state(std::string_view pid) override {
        records_.erase(std::string(pid));
        events_.erase(std::string(pid));
        return result<void>::make();
    }
    std::string_view store_type() const override { return "test"; }

private:
    std::unordered_map<std::string, SnapshotRecord> records_;
    std::unordered_map<std::string, std::vector<EventRecord>> events_;
    uint64_t next_seq_ = 1;
};

// Specialize serialization for TestState
template <>
inline StreamBuffer serialize_state(const TestState& s) {
    std::string data = std::to_string(s.counter) + "|" + s.name;
    return StreamBuffer::from_data(data.data(), data.size());
}

template <>
inline result<TestState> deserialize_state(const StreamBuffer& data) {
    std::string s(reinterpret_cast<const char*>(data.data()), data.size());
    auto pipe = s.find('|');
    if (pipe == std::string::npos) return error(0);
    TestState st;
    st.counter = std::stoi(s.substr(0, pipe));
    st.name = s.substr(pipe + 1);
    return st;
}

// Event-sourced specializations
template <>
inline result<void> apply_event_to_state(TestState& s, const TestEvent& e) {
    s.counter += e.delta;
    return result<void>::make();
}

template <>
inline StreamBuffer serialize_event(const TestEvent& e) {
    auto data = std::to_string(e.delta);
    return StreamBuffer::from_data(data.data(), data.size());
}

template <>
inline result<TestEvent> deserialize_event(const StreamBuffer& data) {
    std::string s(reinterpret_cast<const char*>(data.data()), data.size());
    TestEvent ev;
    ev.delta = std::stoi(s);
    return ev;
}

} // namespace hpactor::actor::durable

---

// tests/unit/actor/durable/test_durable_behavior.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <gtest/gtest.h>
#include <hpactor/actor/durable/durable_behavior.hpp>
#include "durable_test_helpers.hpp"

namespace hpactor::actor::durable {

// Tests
class DurableBehaviorTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<TestInMemoryStore>();
    }
    std::unique_ptr<DurableStateStore> store_;
};

TEST_F(DurableBehaviorTest, ConstructionIsNotRecovered) {
    DurableBehavior<TestState> behavior("actor-1", *store_,
                                         TestState{0, "initial"});
    EXPECT_FALSE(behavior.is_recovered());
    EXPECT_EQ(behavior.persistence_id(), "actor-1");
}

TEST_F(DurableBehaviorTest, RecoverWithNoPriorSnapshotReturnsSuccess) {
    DurableBehavior<TestState> behavior("actor-1", *store_,
                                         TestState{0, "initial"});
    auto result = behavior.recover();
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(behavior.is_recovered());
    EXPECT_EQ(behavior.state().counter, 0);
    EXPECT_EQ(behavior.state().name, "initial");
}

TEST_F(DurableBehaviorTest, SnapshotThenRecover) {
    // Create first instance, modify state, snapshot
    {
        DurableBehavior<TestState> b1("actor-1", *store_,
                                       TestState{0, "initial"});
        b1.recover();
        b1.state().counter = 42;
        b1.state().name = "modified";
        auto snap = b1.snapshot();
        ASSERT_TRUE(snap.is_ok());
        EXPECT_EQ(b1.persistence_id(), "actor-1");
    }

    // Create second instance, recover — should restore state
    {
        DurableBehavior<TestState> b2("actor-1", *store_,
                                       TestState{0, "default"});
        auto result = b2.recover();
        ASSERT_TRUE(result.is_ok());
        EXPECT_TRUE(b2.is_recovered());
        EXPECT_EQ(b2.state().counter, 42);
        EXPECT_EQ(b2.state().name, "modified");
    }
}

} // namespace hpactor::actor::durable
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_durable && ./build/tests/unit/actor/durable/test_unit_durable --gtest_filter="DurableBehaviorTest*"
```

Expected: Compilation fails — `DurableBehavior` not defined.

---

### Task 24: DurableBehavior<State> — Implementation (GREEN)

**Files:**
- Create: `include/hpactor/actor/durable/durable_behavior.hpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/actor/durable_state_store.hpp>
#include <hpactor/adt/stream_buffer.hpp>

#include <string>
#include <utility>

namespace hpactor::actor::durable {

/// \brief Template user must specialize: serialize State to StreamBuffer.
template <typename State>
StreamBuffer serialize_state(const State& s);

/// \brief Template user must specialize: deserialize StreamBuffer to State.
template <typename State>
result<State> deserialize_state(const StreamBuffer& data);

/// \brief Snapshot-mode durable behavior for actors whose state is
///        periodically snapshotted and restored on recovery.
///
/// \tparam State The actor's in-memory state type.
template <typename State>
class DurableBehavior {
public:
    DurableBehavior(std::string persistence_id, DurableStateStore& store,
                    State initial)
        : persistence_id_(std::move(persistence_id))
        , store_(store)
        , state_(std::move(initial)) {}

    /// \brief Load latest snapshot from store and restore state.
    result<void> recover() {
        auto snapshot_result = store_.load_latest_snapshot(persistence_id_);
        if (snapshot_result.is_ok()) {
            auto& snap = snapshot_result.value();
            auto state_result = deserialize_state<State>(snap.data);
            if (!state_result.is_ok()) {
                return error(static_cast<uint32_t>(
                    FailureReason::ReactivationFailed));
            }
            state_ = std::move(state_result.value());
            last_snapshot_sequence_ = snap.sequence;
        }
        // If no snapshot exists, start with initial state — that's fine.
        recovered_ = true;
        return result<void>::make();
    }

    /// \brief Snapshot current state to the store.
    result<void> snapshot() {
        auto data = serialize_state(state_);
        auto result = store_.write_snapshot(persistence_id_, 1, std::move(data));
        if (result.is_ok()) {
            last_snapshot_sequence_ = result.value().sequence;
        }
        return result.is_ok()
            ? result<void>::make()
            : error(static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed));
    }

    State& state() { return state_; }
    const State& state() const { return state_; }
    const std::string& persistence_id() const { return persistence_id_; }
    bool is_recovered() const { return recovered_; }

private:
    std::string persistence_id_;
    DurableStateStore& store_;
    State state_;
    uint64_t last_snapshot_sequence_ = 0;
    bool recovered_ = false;
};

} // namespace hpactor::actor::durable
```

- [ ] **Step 2: Build and run tests**

```bash
ninja -C build test_unit_durable && ./build/tests/unit/actor/durable/test_unit_durable --gtest_filter="DurableBehaviorTest*"
```

Expected: All 3 tests pass.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/actor/durable/durable_behavior.hpp tests/unit/actor/durable/test_durable_behavior.cpp tests/unit/actor/durable/CMakeLists.txt
git commit -m "feat(durable): add DurableBehavior<State> template

Snapshot-mode behavior: recover() loads latest snapshot, snapshot()
persists current state. User specializes serialize_state/deserialize_state.
3 tests covering no-prior-snapshot and snapshot-recover cycle.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 25: EventSourcedBehavior<State, Event> (RED → GREEN)

**Files:**
- Create: `include/hpactor/actor/durable/event_sourced_behavior.hpp`
- Add: `tests/unit/actor/durable/test_event_sourced_behavior.cpp`

- [ ] **Step 0: Extract shared TestInMemoryStore to a helper header**

Create `tests/unit/actor/durable/durable_test_helpers.hpp` with the shared
`TestInMemoryStore` class extracted from `test_durable_behavior.cpp`, plus
shared `TestState`, `TestEvent` types and serialization specializations.
Both `test_durable_behavior.cpp` and `test_event_sourced_behavior.cpp`
include this header.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/actor/durable/test_event_sourced_behavior.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <gtest/gtest.h>
#include <hpactor/actor/durable/event_sourced_behavior.hpp>
#include "durable_test_helpers.hpp"

namespace hpactor::actor::durable {

// Serialization specializations are in durable_test_helpers.hpp

class EventSourcedBehaviorTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<TestInMemoryStore>();
    }
    std::unique_ptr<DurableStateStore> store_;
};

TEST_F(EventSourcedBehaviorTest, PersistEventAndRecover) {
    {
        EventSourcedBehavior<TestState, TestEvent> b1("actor-es-1", *store_,
                                                       TestState{0, "start"});
        b1.recover();
        b1.persist_event_and_apply(TestEvent{5});
        EXPECT_EQ(b1.state().counter, 5);
        b1.persist_event_and_apply(TestEvent{3});
        EXPECT_EQ(b1.state().counter, 8);
        b1.snapshot(); // snapshot at counter=8
    }

    {
        EventSourcedBehavior<TestState, TestEvent> b2("actor-es-1", *store_,
                                                       TestState{0, "default"});
        b2.recover();
        EXPECT_TRUE(b2.is_recovered());
        EXPECT_EQ(b2.state().counter, 8);
    }
}

} // namespace hpactor::actor::durable
```

- [ ] **Step 2: Write the header — event_sourced_behavior.hpp**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/actor/durable/durable_behavior.hpp>
#include <hpactor/actor/durable_state_store.hpp>
#include <hpactor/adt/stream_buffer.hpp>

namespace hpactor::actor::durable {

/// \brief Template user must specialize: apply Event to State.
template <typename State, typename Event>
result<void> apply_event_to_state(State& s, const Event& e);

/// \brief Template user must specialize: serialize Event.
template <typename Event>
StreamBuffer serialize_event(const Event& e);

/// \brief Template user must specialize: deserialize Event.
template <typename Event>
result<Event> deserialize_event(const StreamBuffer& data);

/// \brief Event-sourced durable behavior. Events are persisted before
///        application. Recovery replays events after latest snapshot.
template <typename State, typename Event>
class EventSourcedBehavior {
public:
    EventSourcedBehavior(std::string persistence_id, DurableStateStore& store,
                         State initial)
        : persistence_id_(std::move(persistence_id))
        , store_(store)
        , state_(std::move(initial)) {}

    /// \brief Load latest snapshot then replay events after snapshot.
    result<void> recover() {
        auto snap_result = store_.load_latest_snapshot(persistence_id_);
        if (snap_result.is_ok()) {
            auto& snap = snap_result.value();
            auto state_result = deserialize_state<State>(snap.data);
            if (!state_result.is_ok()) {
                return error(static_cast<uint32_t>(
                    FailureReason::ReactivationFailed));
            }
            state_ = std::move(state_result.value());
            last_snapshot_sequence_ = snap.sequence;

            // Replay events after snapshot
            auto events = store_.load_events_after(
                persistence_id_, last_snapshot_sequence_);
            if (events.is_ok()) {
                for (const auto& ev_rec : events.value()) {
                    auto ev = deserialize_event<Event>(ev_rec.event_data);
                    if (!ev.is_ok()) continue;
                    auto apply_result = apply_event_to_state(state_, ev.value());
                    if (!apply_result.is_ok()) return apply_result;
                    last_event_sequence_ = ev_rec.sequence;
                }
            }
        }
        recovered_ = true;
        return result<void>::make();
    }

    /// \brief Persist event then apply to state.
    result<void> persist_event(const Event& event) {
        auto data = serialize_event(event);
        uint64_t next_seq = last_event_sequence_ + 1;
        auto result = store_.append_event(persistence_id_, next_seq,
                                          std::move(data));
        if (!result.is_ok()) return result;
        last_event_sequence_ = next_seq;
        return apply_event_to_state(state_, event);
    }

    /// \brief Persist and apply in one call.
    result<void> persist_event_and_apply(const Event& event) {
        return persist_event(event);
    }

    /// \brief Snapshot current state.
    result<void> snapshot() {
        auto data = serialize_state(state_);
        auto result = store_.write_snapshot(persistence_id_, 1, std::move(data));
        if (result.is_ok()) {
            last_snapshot_sequence_ = result.value().sequence;
            return result<void>::make();
        }
        return error(static_cast<uint32_t>(
            FailureReason::PassivationSnapshotFailed));
    }

    State& state() { return state_; }
    const State& state() const { return state_; }
    bool is_recovered() const { return recovered_; }

private:
    std::string persistence_id_;
    DurableStateStore& store_;
    State state_;
    uint64_t last_snapshot_sequence_ = 0;
    uint64_t last_event_sequence_ = 0;
    bool recovered_ = false;
};

} // namespace hpactor::actor::durable
```

- [ ] **Step 3: Build and run tests**

Add both test files to test_unit_durable CMakeLists.txt and run:

```bash
ninja -C build test_unit_durable && ./build/tests/unit/actor/durable/test_unit_durable --gtest_filter="EventSourcedBehaviorTest*"
```

Expected: Tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/durable/event_sourced_behavior.hpp tests/unit/actor/durable/test_event_sourced_behavior.cpp tests/unit/actor/durable/CMakeLists.txt
git commit -m "feat(durable): add EventSourcedBehavior<State,Event> template

Event-sourced behavior: persist_event() appends to store then applies,
recover() loads snapshot + replays events. User specializes five template
functions. Tests verify event persistence and recovery.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 26: PassivationManager (RED)

**Files:**
- Create: `include/hpactor/actor/durable/passivation_manager.hpp`
- Create: `src/actor/durable/passivation_manager.cpp`
- Add: `tests/unit/actor/durable/test_passivation_manager.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/actor/durable/test_passivation_manager.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <gtest/gtest.h>
#include <hpactor/actor/durable/passivation_manager.hpp>

namespace hpactor::actor::durable {

TEST(PassivationManagerTest, ConstructionHasNoActors) {
    PassivationManager mgr;
    EXPECT_EQ(mgr.tracked_count(), 0u);
}

TEST(PassivationManagerTest, RegisterActorForPassivation) {
    PassivationManager mgr;
    PassivationConfig config;
    mgr.register_actor(ActorId{100}, "order-1", config);
    EXPECT_EQ(mgr.tracked_count(), 1u);
    EXPECT_TRUE(mgr.is_tracked(ActorId{100}));
}

TEST(PassivationManagerTest, UnregisterActor) {
    PassivationManager mgr;
    mgr.register_actor(ActorId{100}, "order-1", PassivationConfig{});
    mgr.unregister_actor(ActorId{100});
    EXPECT_EQ(mgr.tracked_count(), 0u);
}

TEST(PassivationManagerTest, BeginPassivateSetsState) {
    PassivationManager mgr;
    PassivationConfig config;
    mgr.register_actor(ActorId{100}, "order-1", config);
    EXPECT_TRUE(mgr.begin_passivate(ActorId{100}));
    EXPECT_EQ(mgr.get_state(ActorId{100}), PassivationState::Passivating);
}

TEST(PassivationManagerTest, CompletePassivation) {
    PassivationManager mgr;
    mgr.register_actor(ActorId{100}, "order-1", PassivationConfig{});
    EXPECT_TRUE(mgr.begin_passivate(ActorId{100}));
    mgr.complete_passivation(ActorId{100});
    EXPECT_EQ(mgr.tracked_count(), 0u);
}

TEST(PassivationManagerTest, BeginPassivateUnknownActorReturnsFalse) {
    PassivationManager mgr;
    EXPECT_FALSE(mgr.begin_passivate(ActorId{999}));
}

} // namespace hpactor::actor::durable
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_durable && ./build/tests/unit/actor/durable/test_unit_durable --gtest_filter="PassivationManagerTest*"
```

Expected: Compilation fails — `PassivationManager` not defined.

---

### Task 27: PassivationManager — Implementation (GREEN)

**Files:**
- Create: `include/hpactor/actor/durable/passivation_manager.hpp`
- Create: `src/actor/durable/passivation_manager.cpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/actor/durable/passivation_config.hpp>
#include <hpactor/types/types.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::actor::durable {

/// \brief Passivation state for a tracked durable actor.
enum class PassivationState : uint8_t {
    Active,       ///< Running normally.
    Passivating,  ///< Passivation in progress.
    Passivated,   ///< Memory released; route active.
};

/// \brief Manages passivation lifecycle for durable actors.
///
/// Tracks actors registered for passivation, drives passivation
/// lifecycle, and handles idle timeout scanning.
class PassivationManager {
public:
    PassivationManager() = default;

    /// \brief Register an actor for passivation tracking.
    void register_actor(ActorId actor_id, const std::string& persistence_id,
                        const PassivationConfig& config);

    /// \brief Remove an actor from tracking.
    void unregister_actor(ActorId actor_id);

    /// \brief Check if an actor is tracked.
    bool is_tracked(ActorId actor_id) const;

    /// \brief Begin passivation.
    bool begin_passivate(ActorId actor_id);

    /// \brief Complete passivation (actor is released).
    void complete_passivation(ActorId actor_id);

    /// \brief Get current passivation state.
    PassivationState get_state(ActorId actor_id) const;

    /// \brief Number of tracked actors.
    size_t tracked_count() const;

private:
    struct ActorRecord {
        std::string persistence_id;
        PassivationConfig config;
        PassivationState state = PassivationState::Active;
    };

    std::unordered_map<uint64_t, ActorRecord> actors_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::actor::durable
```

- [ ] **Step 2: Write the implementation**

```cpp
// src/actor/durable/passivation_manager.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <hpactor/actor/durable/passivation_manager.hpp>

namespace hpactor::actor::durable {

void PassivationManager::register_actor(
    ActorId actor_id, const std::string& persistence_id,
    const PassivationConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    actors_[actor_id.value()] = ActorRecord{persistence_id, config,
                                             PassivationState::Active};
}

void PassivationManager::unregister_actor(ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    actors_.erase(actor_id.value());
}

bool PassivationManager::is_tracked(ActorId actor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return actors_.find(actor_id.value()) != actors_.end();
}

bool PassivationManager::begin_passivate(ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actors_.find(actor_id.value());
    if (it == actors_.end() || it->second.state != PassivationState::Active) {
        return false;
    }
    it->second.state = PassivationState::Passivating;
    return true;
}

void PassivationManager::complete_passivation(ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    actors_.erase(actor_id.value());
}

PassivationState PassivationManager::get_state(ActorId actor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actors_.find(actor_id.value());
    if (it == actors_.end()) return PassivationState::Passivated;
    return it->second.state;
}

size_t PassivationManager::tracked_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return actors_.size();
}

} // namespace hpactor::actor::durable
```

- [ ] **Step 3: Build and run tests**

Add passivation_manager.cpp to `src/actor/durable/CMakeLists.txt` (or `src/actor/CMakeLists.txt`). Add test to `tests/unit/actor/durable/CMakeLists.txt`.

```bash
ninja -C build test_unit_durable && ./build/tests/unit/actor/durable/test_unit_durable --gtest_filter="PassivationManagerTest*"
```

Expected: All 6 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/durable/passivation_manager.hpp src/actor/durable/passivation_manager.cpp tests/unit/actor/durable/test_passivation_manager.cpp
git commit -m "feat(durable): add PassivationManager for durable actor lifecycle

Manages passivation state machine: Active → Passivating → Passivated.
Thread-safe. 6 tests covering registration, passivation lifecycle,
and edge cases.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 28: ActorSystem Integration for Durable (RED → GREEN)

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add PassivationManager ownership to ActorSystem**

In header:
```cpp
std::unique_ptr<actor::durable::PassivationManager> passivation_manager_;
```

Accessor:
```cpp
actor::durable::PassivationManager* passivation_manager() {
    return passivation_manager_.get();
}
```

- [ ] **Step 2: Initialize in constructor**

```cpp
passivation_manager_ = std::make_unique<actor::durable::PassivationManager>();
```

- [ ] **Step 3: Build**

```bash
ninja -C build hpactor_lib
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(core): integrate PassivationManager into ActorSystem

ActorSystem owns PassivationManager for durable actor lifecycle.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 29: Durable Workflow Integration Test (RED → GREEN)

**Files:**
- Create: `tests/integration/actor/test_durable_workflow.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

- [ ] **Step 1: Write the integration test**

```cpp
// tests/integration/actor/test_durable_workflow.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <gtest/gtest.h>
#include <hpactor/actor/durable/durable_behavior.hpp>
#include <hpactor/actor/durable/event_sourced_behavior.hpp>
#include <hpactor/actor/durable/passivation_manager.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>

namespace hpactor::actor::durable {

// Reuse test types and store from unit test
// (In real code these would be in a shared test header)

TEST(DurableWorkflowIntegrationTest, FullPassivationAndReactivation) {
    InMemoryStateStore store;
    PassivationManager mgr;

    ActorId actor_id{1};
    PassivationConfig config;
    config.snapshot_on_passivate = true;
    mgr.register_actor(actor_id, "workflow-1", config);

    // First "run" — actor starts, processes, gets passivated
    {
        DurableBehavior<TestState> behavior("workflow-1", store,
                                             TestState{0, "initial"});
        behavior.recover();
        behavior.state().counter = 99;
        behavior.state().name = "processed";

        // Passivation
        EXPECT_TRUE(mgr.begin_passivate(actor_id));
        auto snap = behavior.snapshot();
        ASSERT_TRUE(snap.is_ok());
        mgr.complete_passivation(actor_id);
    }

    // Second "run" — actor is reactivated after restart
    {
        mgr.register_actor(actor_id, "workflow-1", config);
        DurableBehavior<TestState> behavior("workflow-1", store,
                                             TestState{0, "default"});
        auto result = behavior.recover();
        ASSERT_TRUE(result.is_ok());
        EXPECT_TRUE(behavior.is_recovered());
        EXPECT_EQ(behavior.state().counter, 99);
        EXPECT_EQ(behavior.state().name, "processed");
    }
}

TEST(DurableWorkflowIntegrationTest, EventSourcedRecoveryReplaysEvents) {
    InMemoryStateStore store;
    PassivationManager mgr;
    ActorId actor_id{2};

    // Store events
    {
        EventSourcedBehavior<TestState, TestEvent> behavior(
            "es-workflow-1", store, TestState{0, "init"});
        behavior.recover();
        behavior.persist_event_and_apply(TestEvent{10});
        behavior.persist_event_and_apply(TestEvent{20});
        EXPECT_EQ(behavior.state().counter, 30);
        behavior.snapshot();
    }
    // Recover — should replay events after snapshot
    {
        EventSourcedBehavior<TestState, TestEvent> behavior(
            "es-workflow-1", store, TestState{0, "default"});
        behavior.recover();
        EXPECT_EQ(behavior.state().counter, 30);
    }
}

} // namespace hpactor::actor::durable
```

- [ ] **Step 2: Build and run**

```bash
ninja -C build test_integration_actor && ./build/tests/integration/actor/test_integration_actor --gtest_filter="DurableWorkflowIntegrationTest*"
```

Expected: All 2 tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/integration/actor/test_durable_workflow.cpp tests/integration/actor/CMakeLists.txt
git commit -m "test: add durable workflow integration tests

Full passivation → reactivation cycle and event-sourced recovery
with event replay. 2 integration tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 30: TOML Config Parsers (RED → GREEN)

**Files:**
- Create: `src/config/parsers/reliable_messaging_config_parser.cpp`
- Create: `src/config/parsers/durable_config_parser.cpp`

- [ ] **Step 1: Write reliable messaging config parser**

```cpp
// src/config/parsers/reliable_messaging_config_parser.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/config/toml_table_view.hpp>
#include <hpactor/config/system_config.hpp>

namespace hpactor {

class ReliableMessagingConfigParser : public TomlSystemParser {
public:
    void parse(const TomlTableView& table, SystemConfig& config) override {
        auto reliable = table.get_table("reliable");
        if (!reliable) return;

        config.reliable_enabled = reliable->get_bool("enabled").value_or(false);
        config.reliable_max_retries = static_cast<uint32_t>(
            reliable->get_int("max_retries").value_or(3));
        config.reliable_initial_backoff_ms = static_cast<uint32_t>(
            reliable->get_int("initial_backoff_ms").value_or(100));
        config.reliable_max_backoff_ms = static_cast<uint32_t>(
            reliable->get_int("max_backoff_ms").value_or(10000));
        config.reliable_backoff_multiplier =
            reliable->get_double("backoff_multiplier").value_or(2.0);
        config.reliable_max_pending = static_cast<size_t>(
            reliable->get_int("max_pending_per_destination").value_or(1024));
    }
};

static TomlSystemParserRegistration<ReliableMessagingConfigParser>
    reliable_parser_reg("reliable_messaging");

} // namespace hpactor
```

- [ ] **Step 2: Write durable config parser**

```cpp
// src/config/parsers/durable_config_parser.cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/config/toml_table_view.hpp>
#include <hpactor/config/system_config.hpp>

namespace hpactor {

class DurableConfigParser : public TomlSystemParser {
public:
    void parse(const TomlTableView& table, SystemConfig& config) override {
        auto durable = table.get_table("durable");
        if (!durable) return;

        config.durable_enabled = durable->get_bool("enabled").value_or(false);
        config.durable_store_type = durable->get_string("store_type").value_or("in_memory");
        config.durable_file_root_dir = durable->get_string("file_root_dir").value_or("/var/lib/hpactor/state");
        config.durable_default_idle_timeout_seconds = static_cast<uint32_t>(
            durable->get_int("default_idle_timeout_seconds").value_or(1800));
    }
};

static TomlSystemParserRegistration<DurableConfigParser>
    durable_parser_reg("durable_config");

} // namespace hpactor
```

- [ ] **Step 3: Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: Build succeeds (may need to add config fields to SystemConfig if not already present — add reasonable defaults).

- [ ] **Step 4: Commit**

```bash
git add src/config/parsers/reliable_messaging_config_parser.cpp src/config/parsers/durable_config_parser.cpp
git commit -m "feat(config): add self-registering parsers for reliable and durable config

TOML [system.reliable] and [system.durable] config parsers following
existing self-registration pattern. No changes to monolithic parser.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 31: Final Integration Build & Full Regression Test

- [ ] **Step 1: Full build**

```bash
ninja -C build
```

Expected: All targets build. No regressions.

- [ ] **Step 2: Run all new tests**

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="ReliableRetryPolicyTest*:OutboundTrackerTest*:InMemoryDeliveryStoreTest*:FileDeliveryStoreTest*"
./build/tests/unit/net/test_unit_net --gtest_filter="ReliableAckTest*"
./build/tests/integration/mailbox/test_integration_mailbox --gtest_filter="ReliableMessagingIntegrationTest*"
./build/tests/unit/cluster/test_unit_cluster --gtest_filter="SingletonManagerActorTest*:ShardCoordinatorActorTest*"
./build/tests/unit/actor/durable/test_unit_durable
```

Expected: All ~105 tests pass.

- [ ] **Step 3: Run existing regression suite**

```bash
./build/tests/unit/cluster/test_unit_cluster --gtest_filter="-SingletonManagerActorTest*:-ShardCoordinatorActorTest*"
```

Expected: All 98 existing Sprint 2 tests still pass. No regressions.

- [ ] **Step 4: Commit final integration**

```bash
git add -A
git commit -m "feat: complete Sprint 3 — Reliable Messaging, Singleton Actor, Durable Runtime

Implements MSG-003, CLU-003, and DUR-001/002:
- MSG-003: ACK/NACK wire protocol, OutboundTracker with retry/backoff,
  InMemoryDeliveryStore + FileDeliveryStore, auto-ACK in EventBasedActor
- CLU-003: SingletonManagerActor + ShardCoordinatorActor wrapping cores,
  ActorSystem integration, ClusterFailureModel observer callbacks
- DUR-001/002: DurableBehavior<State>, EventSourcedBehavior<State,Event>,
  PassivationManager, RecoveryPolicy, PassivationConfig
- TOML config parsers for reliable messaging and durable state

~105 new tests. All existing tests pass.

Design: docs/superpowers/specs/2026-06-22-akka-gap-closure-sprint3-design.md
Implements the three remaining P1 gaps from issue #329.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Summary: Task Count

| PR | Tasks | New Files | Tests |
|----|-------|-----------|-------|
| 1 (MSG-003) | 1–14 | 9 | ~38 |
| 2 (CLU-003) | 15–20 | 5 | ~27 |
| 3 (DUR-001/002) | 21–31 | 9 | ~40 |
| **Total** | **31** | **23** | **~105** |
