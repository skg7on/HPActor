# Graceful Actor Stop Protocol — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement per-actor graceful stop with drain policies, timeout enforcement, DLQ integration, and node-level shutdown coordinator.

**Architecture:** Extend `LifecycleActor` with `DrainConfig` and `on_drain_timeout()` hook. Add drain-aware processing to `EventBasedActor::receive()` that checks drain state on each dequeue. `ActorContext::stop()` triggers individual actor drain; `ActorSystem::shutdown()` drives a phase machine that drains all actors in reverse topological order. DLQ writes go through existing `ActorSystem::dead_letter()`.

**Tech Stack:** C++20, TimingWheel for timeout, existing LifecycleActor state machine, existing mailbox::DeadLetterQueue, self-registering TOML parser pattern, existing CLI command tree.

---

### Task 1: DeadLetterReason — Add drain-specific values

**Files:**
- Modify: `include/hpactor/mailbox/dead_letter_queue.hpp:27-39`
- Modify: `src/mailbox/dead_letter_queue.cpp` (if any to_string/switch is present)

- [ ] **Step 1: Add new enum values**

In `include/hpactor/mailbox/dead_letter_queue.hpp`, add two new values to `DeadLetterReason`:

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
    DrainTimeout = 12,      // NEW: message dropped because drain deadline expired
    DrainPolicyDrop = 13,   // NEW: message dropped by DropUserMessages policy
};
```

- [ ] **Step 2: Check for switch/string conversion**

Check if `src/mailbox/dead_letter_queue.cpp` has a `to_string()` or switch that needs updating:

```bash
grep -n "switch\|to_string\|case " src/mailbox/dead_letter_queue.cpp | head -20
```

If a switch or string table exists, add cases for `DrainTimeout` and `DrainPolicyDrop`.

- [ ] **Step 3: Build and verify compilation**

```bash
cmake -S . -B build -GNinja && ninja -C build
```

Expected: clean build, no errors about unhandled enum values.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/dead_letter_queue.hpp src/mailbox/dead_letter_queue.cpp
git commit -m "feat(dlq): add DrainTimeout and DrainPolicyDrop reasons"
```

---

### Task 2: DrainPolicy + DrainConfig types

**Files:**
- Create: `include/hpactor/actor/drain_policy.hpp`
- Create: `include/hpactor/actor/drain_config.hpp`

- [ ] **Step 1: Create DrainPolicy enum**

Create `include/hpactor/actor/drain_policy.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace hpactor {

enum class DrainPolicy : uint8_t {
    Drain = 0,            // Process all mailbox messages before stopping
    DropUserMessages = 1, // Dead-letter user messages, keep system messages
    ImmediateStop = 2,    // Stop immediately, dead-letter everything
    SnapshotAndStop = 3,  // [DEFERRED] Durable actors persist state then stop
    TransferShard = 4,    // [DEFERRED] Sharded actors hand off ownership
};

} // namespace hpactor
```

- [ ] **Step 2: Create DrainConfig struct**

Create `include/hpactor/actor/drain_config.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/actor/drain_policy.hpp>
#include <chrono>

namespace hpactor {

struct DrainConfig {
    DrainPolicy policy{DrainPolicy::Drain};
    std::chrono::milliseconds timeout{30'000};
};

} // namespace hpactor
```

- [ ] **Step 3: Build and verify**

```bash
ninja -C build
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/drain_policy.hpp include/hpactor/actor/drain_config.hpp
git commit -m "feat(actor): add DrainPolicy enum and DrainConfig struct"
```

---

### Task 3: LifecycleActor — Drain support

**Files:**
- Modify: `include/hpactor/actor/lifecycle_actor.hpp`
- Modify: `src/actor/lifecycle_actor.cpp`

- [ ] **Step 1: Add drain members to LifecycleActor**

In `include/hpactor/actor/lifecycle_actor.hpp`, add:

- Include `drain_config.hpp`
- Add `virtual void on_drain_timeout() {}` to virtual hooks
- Add `drain_config() const noexcept` and `set_drain_config(DrainConfig)` accessors
- Add `DrainConfig drain_config_{}` to protected members

```cpp
// In the public virtual hooks section, after on_drain():
virtual void on_drain_timeout() {}

// In the public section, after failure_reason():
DrainConfig drain_config() const noexcept { return drain_config_; }
void set_drain_config(DrainConfig cfg) noexcept { drain_config_ = cfg; }

// In the protected section:
DrainConfig drain_config_{};
```

- [ ] **Step 2: Update transition() to handle drain timeout path**

In `src/actor/lifecycle_actor.cpp`, no changes needed — `on_drain_timeout()` is called by the timer callback, not by `transition()`. Verify this by reading the current transition logic.

- [ ] **Step 3: Build and verify**

```bash
ninja -C build
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/lifecycle_actor.hpp src/actor/lifecycle_actor.cpp
git commit -m "feat(lifecycle): add drain config and on_drain_timeout hook"
```

---

### Task 4: MetricEventType + LogEventId — New drain events

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp`
- Modify: `include/hpactor/log/detail/log_macros.hpp`

- [ ] **Step 1: Add metric event types**

In `include/hpactor/metrics/metrics_event.hpp`, add to `MetricEventType` enum:

```cpp
kLifecycleTransition = 15,
kMessageRejected = 16,
kActorDrainStart = 17,     // NEW
kActorDrainComplete = 18,  // NEW
kActorDrainTimeout = 19,   // NEW
```

- [ ] **Step 2: Add log event IDs**

In `include/hpactor/log/detail/log_macros.hpp`, add to `HPACTOR_LOG_EVENTS`:

```cpp
X(kActorLifecycleTransition, 1004, "actor_lifecycle_transition")  \
X(kActorDrainStart, 1005, "actor_drain_start")                    \
X(kActorDrainComplete, 1006, "actor_drain_complete")              \
X(kActorDrainTimeout, 1007, "actor_drain_timeout")                \
X(kShutdownPhaseTransition, 1008, "shutdown_phase_transition")
```

Note: 1004 was previously unused. Verify no existing event uses 1004.

- [ ] **Step 3: Build and verify**

```bash
ninja -C build
```

Expected: clean build. Any switch/string table using the X-macros will automatically pick up the new entries.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp include/hpactor/log/detail/log_macros.hpp
git commit -m "feat: add drain/shutdown metric events and log IDs"
```

---

### Task 5: is_system_actor() — Virtual on AbstractActor

**Files:**
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `src/actor/spawn_receiver.cpp` (or relevant system actor files)
- Modify: `include/hpactor/cli/cli_actor.hpp`
- Modify: `include/hpactor/metrics/metrics_actor.hpp`

- [ ] **Step 1: Add virtual method to AbstractActor**

In `include/hpactor/actor/abstract_actor.hpp`, add after `as_lifecycle()`:

```cpp
// Returns true for system actors that should drain last during node shutdown.
// MetricsActor, CliActor, SpawnReceiver override this to return true.
virtual bool is_system_actor() const { return false; }
```

- [ ] **Step 2: Override in system actor classes**

Check each system actor class. If they're header-only or have a .cpp, add:

```cpp
bool is_system_actor() const override { return true; }
```

For `SpawnReceiver`, `MetricsActor`, and `CliActor`. Check the exact class definitions:

```bash
grep -rn "class SpawnReceiver\|class MetricsActor\|class CliActor" --include="*.hpp" include/
```

- [ ] **Step 3: Build and verify**

```bash
ninja -C build
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/abstract_actor.hpp
# Add files for any system actor overrides
git commit -m "feat(actor): add is_system_actor() virtual method"
```

---

### Task 6: Drain-aware message processing in EventBasedActor

**Files:**
- Modify: `include/hpactor/actor/event_based_actor.hpp`
- Modify: `src/actor/event_based_actor.cpp`

This is the core drain loop implementation. The approach: during `kDraining` state, `receive()` intercepts each dequeued message and applies the drain policy before normal dispatch. A helper method `drain_one()` encapsulates the per-message drain logic.

- [ ] **Step 1: Write failing tests for drain processing**

Create `tests/actor/test_drain_policy.cpp`:

**Test: drain_policy_processes_all_messages**
- Spawn actor with `DrainPolicy::Drain`, enqueue 3 user + 1 system message
- Trigger drain
- Assert all 4 messages were processed (handler counters incremented)

**Test: drop_user_messages_deadletters_user_keeps_system**
- Spawn actor with `DrainPolicy::DropUserMessages`
- Enqueue 2 user + 1 LinkMsg (system)
- Trigger drain
- Assert LinkMsg was processed, user messages went to DLQ

**Test: immediate_stop_deadletters_all**
- Spawn actor with `DrainPolicy::ImmediateStop`
- Enqueue 2 user + 1 system message
- Trigger drain
- Assert all 3 went to DLQ, none processed

**Test: deferred_policy_falls_back_to_drain**
- Spawn actor with `DrainPolicy::SnapshotAndStop`
- Trigger drain
- Assert log warning emitted, behavior matches Drain

```bash
# After writing test file and updating CMakeLists.txt
cmake -S . -B build -GNinja && ninja -C build test_drain_policy
./build/tests/test_drain_policy
```

Expected: build succeeds, tests FAIL (no drain logic yet).

- [ ] **Step 2: Add drain helper to EventBasedActor**

In `include/hpactor/actor/event_based_actor.hpp`, add:

```cpp
// Drain one message according to the actor's drain policy.
// Returns true if the message was processed (not dead-lettered).
// Returns false if the message was dead-lettered or skipped.
bool drain_one(TypedMessage& msg);
```

- [ ] **Step 3: Implement drain_one() and update receive()**

In `src/actor/event_based_actor.cpp`, implement `drain_one()`:

```cpp
bool EventBasedActor::drain_one(TypedMessage& msg) {
    auto* lc = as_lifecycle();
    if (!lc) return true; // no lifecycle = process normally

    auto policy = lc->drain_config().policy;
    bool is_system = static_cast<uint16_t>(msg.type_id()) < 0x1000;

    switch (policy) {
    case DrainPolicy::Drain:
        return true; // process normally

    case DrainPolicy::DropUserMessages:
        if (!is_system) {
            DeadLetterRecord record;
            record.reason = DeadLetterReason::DrainPolicyDrop;
            record.source = DeadLetterSource::MailboxAdmission;
            record.sender = msg.sender_address();
            record.target = address();
            record.type_tag = msg.type_id();
            auto _ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
    system().clock().now().time_since_epoch()).count();
record.timestamp_ns = static_cast<uint64_t>(_ns);
            system().dead_letter(std::move(record));
            return false;
        }
        return true;

    case DrainPolicy::ImmediateStop:
        // handled at drain trigger, not per-message
        return false;

    case DrainPolicy::SnapshotAndStop:
    case DrainPolicy::TransferShard:
        // Deferred — log warning once and fall back to Drain
        HPACTOR_LOG_WARN(log::LogCategory::kActor, id(),
                         static_cast<uint32_t>(log::LogEventId::kActorDrainStart),
                         "deferred drain policy; falling back to Drain");
        lc->set_drain_config(DrainConfig{DrainPolicy::Drain, lc->drain_config().timeout});
        return true;
    }
    return true;
}
```

Update `KillRequest` handling in `receive()` (around line 226-248 of event_based_actor.cpp) — instead of directly transitioning to `kStopping→kStopped`, trigger the graceful drain path:

```cpp
// KillRequest: drive graceful stop through the lifecycle state machine
if (msg.type_id() == TypeTag::KillRequestTag) {
    // ... existing parse + reply logic ...

    if (auto* lc = as_lifecycle()) {
        // Start graceful drain instead of immediate stop
        lc->transition(LifecycleState::kDraining);
        // The drain loop and on_drain() handle the rest
    } else {
        // No lifecycle — immediate stop
        set_exit_reason(0);
    }
    return;
}
```

Also update the lifecycle message gate in `receive()` to DLQ rejected messages during drain (currently around line 168-174):

```cpp
if (static_cast<uint16_t>(msg.type_id()) >= 0x1000) {
    if (auto* lc = as_lifecycle()) {
        if (!lc->accepts_user_msgs()) {
            // Dead-letter user messages rejected during drain
            if (lc->state() == LifecycleState::kDraining) {
                DeadLetterRecord record;
                record.reason = DeadLetterReason::MailboxClosed;
                record.source = DeadLetterSource::MailboxAdmission;
                record.sender = msg.sender_address();
                record.target = address();
                record.type_tag = msg.type_id();
                auto _ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
    system().clock().now().time_since_epoch()).count();
record.timestamp_ns = static_cast<uint64_t>(_ns);
                system().dead_letter(std::move(record));
            }
            return;
        }
    }
}
```

- [ ] **Step 4: Handle ImmediateStop at drain trigger**

ImmediateStop dead-letters all mailbox messages and skips the drain loop. Add a helper:

```cpp
void EventBasedActor::drain_all_immediate() {
    while (auto* msg = mailbox_->try_dequeue()) {
        DeadLetterRecord record;
        record.reason = DeadLetterReason::MailboxClosed;
        record.source = DeadLetterSource::MailboxAdmission;
        record.sender = msg->sender_address();
        record.target = address();
        record.type_tag = msg->type_id();
        auto _ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
    system().clock().now().time_since_epoch()).count();
record.timestamp_ns = static_cast<uint64_t>(_ns);
        system().dead_letter(std::move(record));
    }
}
```

- [ ] **Step 5: Build and run tests**

```bash
ninja -C build test_drain_policy
./build/tests/test_drain_policy
```

Expected: all tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/event_based_actor.hpp src/actor/event_based_actor.cpp tests/actor/test_drain_policy.cpp tests/actor/CMakeLists.txt
git commit -m "feat(drain): add drain-aware message processing in EventBasedActor"
```

---

### Task 7: Drain timeout enforcement

**Files:**
- Modify: `src/actor/event_based_actor.cpp`
- Create: `tests/actor/test_drain_timeout.cpp`

- [ ] **Step 1: Write failing timeout test**

Create `tests/actor/test_drain_timeout.cpp`:

**Test: drain_timeout_forces_transition**
- Spawn actor with `DrainConfig{Drain, timeout=1ms}`
- Enqueue many messages (make drain take time)
- Trigger drain
- Wait for timeout
- Assert lifecycle state is `kStopping` (not still `kDraining`)

**Test: drain_completes_before_timeout_cancels_timer**
- Spawn actor with `DrainConfig{Drain, timeout=5000ms}`
- Enqueue 1 message
- Trigger drain
- Assert drain completes naturally, state is `kStopped`

```bash
cmake -S . -B build -GNinja && ninja -C build test_drain_timeout
./build/tests/test_drain_timeout
```

Expected: FAIL (no timeout logic yet).

- [ ] **Step 2: Implement timer scheduling in drain trigger**

When drain is triggered (via `ActorContext::stop()` — implemented in Task 8), schedule a one-shot timer. The drain trigger needs access to the scheduler. Add a method:

```cpp
void EventBasedActor::start_drain_timer() {
    auto* lc = as_lifecycle();
    if (!lc || !scheduler_) return;

    auto timeout = lc->drain_config().timeout;
    auto* actor_ptr = this;
    scheduler_->schedule_after(timeout, [actor_ptr]() {
        if (auto* lc2 = actor_ptr->as_lifecycle()) {
            if (lc2->state() == LifecycleState::kDraining) {
                lc2->on_drain_timeout();
                // Dead-letter remaining mailbox messages
                actor_ptr->drain_all_immediate();
                lc2->transition(LifecycleState::kStopping);
                lc2->transition(LifecycleState::kStopped);
                actor_ptr->on_exit();
            }
        }
    });
}
```

- [ ] **Step 3: Build and run timeout tests**

```bash
ninja -C build test_drain_timeout
./build/tests/test_drain_timeout
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/actor/event_based_actor.cpp tests/actor/test_drain_timeout.cpp tests/actor/CMakeLists.txt
git commit -m "feat(drain): implement drain timeout via TimingWheel"
```

---

### Task 8: ActorContext::stop() and stop_sync()

**Files:**
- Modify: `include/hpactor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`
- Create: `tests/actor/test_actor_stop.cpp`

- [ ] **Step 1: Write failing test**

Create `tests/actor/test_actor_stop.cpp`:

**Test: stop_async_transitions_to_stopped**
- Spawn actor via ActorSystem
- Call `context()->stop(actor->id())`
- Poll until actor state is `kStopped`
- Assert DownMsg was sent to linked actors

**Test: stop_sync_blocks_until_stopped**
- Spawn actor with empty mailbox
- Call `stop_sync(id, 5s)`
- Assert returns ok, actor is stopped

**Test: stop_sync_timeout_returns_error**
- Spawn actor with never-emptying mailbox
- Call `stop_sync(id, 10ms)`
- Assert returns error

- [ ] **Step 2: Add declarations to ActorContext**

In `include/hpactor/actor_context.hpp`, add:

```cpp
// Graceful actor stop — initiates drain per actor's DrainPolicy.
// Returns immediately; the actor drains on its scheduler thread.
void stop(ActorId target);

// Synchronous stop — blocks until target reaches Stopped or timeout.
// Returns error on timeout. Do not call from actor threads.
result<void> stop_sync(ActorId target, std::chrono::milliseconds timeout);
```

- [ ] **Step 3: Implement stop() and stop_sync()**

In `src/actor/actor_context.cpp`:

`stop()`:
1. Resolve target actor via `system_->get_actor(target)`
2. Get its `LifecycleActor*` via `as_lifecycle()`
3. If no lifecycle, set exit reason and call `on_exit()` directly
4. If `ImmediateStop` policy, drain mailbox immediately, then `transition(kStopping)→kStopped`
5. Otherwise, `transition(kDraining)` → `on_drain()` → schedule drain timer
6. Emit `kActorDrainStart` metric event

`stop_sync()`:
1. Call `stop(target)`
2. Loop with sleep polling actor state until `kStopped` or timeout
3. Return `result<void>` — ok on stopped, error on timeout

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_actor_stop
./build/tests/test_actor_stop
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/actor_context.hpp src/actor/actor_context.cpp tests/actor/test_actor_stop.cpp tests/actor/CMakeLists.txt
git commit -m "feat(actor): add ActorContext::stop() and stop_sync()"
```

---

### Task 9: TOML shutdown config parser

**Files:**
- Create: `src/config/parsers/shutdown_config_parser.cpp`
- Modify: `include/hpactor/config/topology_model.hpp` (add ShutdownConfig to SystemDef)
- Modify: `include/hpactor/core/actor_system.hpp` (add ShutdownConfig to Config)
- Create: `tests/config/test_shutdown_config.cpp`

- [ ] **Step 1: Add ShutdownConfig to Config and SystemDef**

In `include/hpactor/core/actor_system.hpp`, add to `Config` struct (after dead_letters around line 142):

```cpp
// Shutdown configuration
DrainConfig shutdown_drain{DrainPolicy::Drain, std::chrono::milliseconds{30'000}};
uint32_t ingress_timeout_ms{5000};
uint32_t cluster_leave_timeout_ms{10000};
bool shutdown_force_after_timeout{true};
```

In `include/hpactor/config/topology_model.hpp`, add to `SystemDef`:

```cpp
std::string default_drain_policy{"Drain"};
uint32_t default_drain_timeout_ms{30000};
uint32_t shutdown_ingress_timeout_ms{5000};
uint32_t shutdown_cluster_leave_timeout_ms{10000};
bool shutdown_force_after_timeout{true};
```

- [ ] **Step 2: Write failing config test**

Create `tests/config/test_shutdown_config.cpp`:

**Test: parse_default_shutdown_config**
- Load TOML with `[system.shutdown]` absent
- Assert defaults: drain_policy=Drain, timeout_ms=30000

**Test: parse_custom_shutdown_config**
- Load TOML with custom shutdown config
- Assert parsed values match

**Test: actor_override_drain_policy**
- Load TOML with per-actor `drain_policy` arg
- Assert actor gets overridden config

- [ ] **Step 3: Create shutdown config parser**

Create `src/config/parsers/shutdown_config_parser.cpp` following the self-registering pattern from `metrics_config_parser.cpp`:

```cpp
#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>

namespace hpactor::config {
namespace {

class ShutdownConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.shutdown";
    static constexpr int kOrder = 110;

    std::string_view name() const noexcept override { return kName; }
    int order() const noexcept override { return kOrder; }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto st = system.table("shutdown");
        if (!st.valid()) return result<void>::make();

        out.default_drain_policy = st.read_string("drain_policy", "Drain");
        out.default_drain_timeout_ms =
            st.read_uint32("drain_timeout_ms", 30000);
        out.shutdown_ingress_timeout_ms =
            st.read_uint32("ingress_timeout_ms", 5000);
        out.shutdown_cluster_leave_timeout_ms =
            st.read_uint32("cluster_leave_timeout_ms", 10000);
        out.shutdown_force_after_timeout =
            st.read_bool("force_after_timeout", true);
        return result<void>::make();
    }
};

const TomlSystemParserRegistration<ShutdownConfigParser>
    kRegisterShutdownConfigParser;

} // anonymous namespace
} // namespace hpactor::config
```

- [ ] **Step 4: Build and run config test**

```bash
ninja -C build test_shutdown_config
./build/tests/test_shutdown_config
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/config/parsers/shutdown_config_parser.cpp tests/config/test_shutdown_config.cpp include/hpactor/config/topology_model.hpp include/hpactor/core/actor_system.hpp
git commit -m "feat(config): add TOML shutdown config parser"
```

---

### Task 10: ActorSystem::shutdown() and ShutdownCoordinator

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Create: `tests/actor/test_shutdown_coordinator.cpp`

- [ ] **Step 1: Write failing coordinator tests**

Create `tests/actor/test_shutdown_coordinator.cpp`:

**Test: shutdown_phase_machine_transitions**
- Spawn a few actors, call `shutdown()`
- Assert phases visited: DrainingIngress → DrainingActors → Stopped

**Test: shutdown_reverse_topological_order**
- Spawn parent→child tree, verify child drains before parent

**Test: shutdown_system_actors_drain_last**
- Spawn user actors + MetricsActor
- Verify MetricsActor drains after all user actors

**Test: forced_stop_on_timeout**
- Spawn actor with very long drain, shutdown with 1ms timeout
- Assert ForcedStop reached, actors terminated

- [ ] **Step 2: Add shutdown API to ActorSystem header**

In `include/hpactor/core/actor_system.hpp`, add:

```cpp
enum class ShutdownPhase : uint8_t {
    Running,
    DrainingIngress,
    DrainingActors,
    LeavingCluster,
    FlushingTelemetry,
    Stopped,
    ForcedStop,
};

struct ShutdownOptions {
    std::chrono::milliseconds ingress_timeout{5'000};
    std::chrono::milliseconds actor_drain_timeout{30'000};
    std::chrono::milliseconds cluster_leave_timeout{10'000};
    bool force_after_timeout{true};
};

// Node shutdown — see design spec for phase machine details
result<void> shutdown(const ShutdownOptions& opts = {});

// Health/readiness gating
bool is_ready() const noexcept;
bool is_draining() const noexcept;

// Per-actor drain config override
void set_drain_config(ActorId target, DrainConfig cfg);
```

- [ ] **Step 3: Implement ShutdownCoordinator**

In `src/actor/actor_system.cpp`, implement `ShutdownCoordinator` as a file-local class:

```cpp
namespace {
class ShutdownCoordinator {
  public:
    ShutdownCoordinator(ActorSystem& sys, const ShutdownOptions& opts);
    result<void> execute();

  private:
    result<void> drain_ingress();
    result<void> drain_actors();
    result<void> leave_cluster();
    result<void> flush_telemetry();

    ActorSystem& system_;
    ShutdownOptions opts_;
    ShutdownPhase phase_{ShutdownPhase::Running};
    std::atomic<bool> force_stop_{false};
};
}
```

`drain_actors()` implementation:
1. Collect all actors via `system_.for_each_actor()`
2. Separate into user actors and system actors (`is_system_actor()`)
3. Sort user actors in reverse topological order (children before parents using post-order traversal of spawn tree)
4. Call `stop()` on each user actor, waiting for drain completion
5. Call `stop()` on each system actor
6. If deadline exceeded and `force_after_timeout`, set `force_stop_` flag

`ActorSystem::shutdown()`:
1. Create `ShutdownCoordinator`
2. Call `execute()`
3. Return result

`is_ready()` — returns `false` when `phase_ >= ShutdownPhase::DrainingIngress`
`is_draining()` — returns `true` when `phase_ == ShutdownPhase::DrainingActors`

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_shutdown_coordinator
./build/tests/test_shutdown_coordinator
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/actor/test_shutdown_coordinator.cpp tests/actor/CMakeLists.txt
git commit -m "feat(shutdown): add ActorSystem::shutdown() with ShutdownCoordinator"
```

---

### Task 11: CLI commands — /system drain, /system stop

**Files:**
- Modify: `src/cli/cli_actor.cpp`

- [ ] **Step 1: Add CLI commands**

In `src/cli/cli_actor.cpp`, in the `build_command_tree()` function, add after existing `/system` commands:

First add a \`shutdown_phase()\` getter to ActorSystem alongside \`is_ready()\`
and \`is_draining()\` (from Task 10):

\`\`\`cpp
ShutdownPhase shutdown_phase() const noexcept;
\`\`\`

Then add the CLI commands:

\`\`\`cpp
// ── /system drain ──────────────────────────────────────────────
auto* drain = sys->add_child("drain", "Graceful node shutdown");
drain->execute = [this](CommandContext& ctx) -> result<void> {
    auto result = system().shutdown();
    if (result.has_value()) {
        ctx.output->raw("Shutdown complete");
    } else {
        ctx.output->error("Shutdown failed");
    }
    return result<void>::make();
};

// ── /system drain status ───────────────────────────────────────
drain->add_child("status", "Show shutdown progress")->execute =
    [this](CommandContext& ctx) -> result<void> {
    ctx.output->raw("Shutdown phase: " +
                    std::to_string(static_cast<int>(system().shutdown_phase())));
    ctx.output->raw("Actors live: " +
                    std::to_string(system().actor_count()));
    return result<void>::make();
};

// ── /system stop <actor-id> ────────────────────────────────────
auto* stop = sys->add_child("stop", "Graceful stop of an actor");
auto* id_param = stop->add_child("<actor_id>", "Actor ID to stop",
                                  /*is_param=*/true);
id_param->execute = [this](CommandContext& ctx) -> result<void> {
    auto id_str = ctx.get_param("<actor_id>");
    if (!id_str) {
        ctx.output->error("Missing actor ID (usage: /system stop <actor_id>)");
        return result<void>::make();
    }
    ActorId target_id{std::stoull(std::string(*id_str))};

    if (ctx.has_flag("force")) {
        system().set_drain_config(target_id,
                                  DrainConfig{DrainPolicy::ImmediateStop});
    }

    auto actor = system().get_actor(target_id);
    if (!actor) {
        ctx.output->error("Actor not found: " + std::string(*id_str));
        return result<void>::make();
    }
    auto* actx = actor->actor_context();
    if (actx) {
        actx->stop(target_id);
        ctx.output->raw("Drain initiated for actor " + std::string(*id_str));
    } else {
        ctx.output->error("Actor has no context");
    }
    return result<void>::make();
};
\`\`\`

- [ ] **Step 2: Build and verify CLI**

```bash
ninja -C build
# Run the CLI actor to verify commands register
./build/tests/test_cli_integration
```

Expected: commands appear in help, no registration errors.

- [ ] **Step 3: Commit**

```bash
git add src/cli/cli_actor.cpp
git commit -m "feat(cli): add /system drain and /system stop commands"
```

---

### Task 12: End-to-end integration test

**Files:**
- Create: `tests/actor/test_drain_integration.cpp`

- [ ] **Step 1: Write integration test**

**Test: full_shutdown_drains_spawn_tree**
- Create ActorSystem with config
- Spawn parent actor with 3 children
- Send messages to all actors
- Call `system.shutdown()`
- Assert all actors reached `kStopped`
- Assert DLQ contains messages dropped per policy

**Test: drain_policy_flows_end_to_end**
- Spawn 3 actors, each with different DrainPolicy (Drain, DropUserMessages, ImmediateStop)
- Enqueue messages to each
- Call `system.shutdown()`
- Verify each actor's messages handled according to policy

- [ ] **Step 2: Run integration test**

```bash
ninja -C build test_drain_integration
./build/tests/test_drain_integration
```

Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add tests/actor/test_drain_integration.cpp tests/actor/CMakeLists.txt
git commit -m "test: add drain/shutdown integration tests"
```

---

### Task 13: Final verification

- [ ] **Step 1: Run full test suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: 100% tests passing. New test count = 128 + 6 new test suites = 134 tests.

- [ ] **Step 2: Check for sanitizer issues**

```bash
cmake -S . -B build_asan -GNinja -DENABLE_ASAN=ON
ninja -C build_asan test_drain_policy test_drain_timeout test_actor_stop test_shutdown_coordinator test_shutdown_config test_drain_integration
```

- [ ] **Step 3: Verify CLI commands via existing test**

```bash
./build/tests/test_cli_integration
```

Expected: all existing CLI tests pass, new commands don't break anything.

- [ ] **Step 4: Final commit**

```bash
git add -A
git commit -m "chore: final verification — all tests passing"
```
