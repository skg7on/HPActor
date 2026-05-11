# Actor Lifecycle State Machine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the opt-in LifecycleActor mixin with 7-state declarative state machine, message gating, and integration into ActorSystem, supervision, CLI, and metrics.

**Architecture:** New `lifecycle_state.hpp` holds the `constexpr StateDef` table where each state owns its transition rules. `LifecycleActor` is a virtual mixin class (multiple inheritance) with atomic state storage, CAS-based transitions, and post-transition hooks. `AbstractActor` gains a virtual `as_lifecycle()` method returning `nullptr` by default; each lifecycle-capable actor must explicitly override it to return `this` (standard RTTI-free mixin downcast). The gate is a single check in `EventBasedActor::receive()` after system message interception.

**Deferred items (explicitly excluded from this plan):**
- CLI `/actor <id> drain` command — requires CLI command tree integration; track as follow-up
- Metrics ring buffer wiring inside `transition()` — `LifecycleActor` has no reference to metrics infrastructure; emit metrics at call sites instead

**Tech Stack:** C++20, `std::atomic`, `-fno-exceptions`, `-fno-rtti`, LLVM coding standards

---

### Task 1: Create lifecycle_state.hpp — enum + StateDef constexpr table

**Files:**
- Create: `include/hpactor/actor/lifecycle_state.hpp`

- [ ] **Step 1: Write the header file**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace hpactor {

enum class LifecycleState : uint8_t {
    kStarting   = 0,
    kActive     = 1,
    kDraining   = 2,
    kStopping   = 3,
    kStopped    = 4,
    kFailed     = 5,
    kRecovering = 6,
};

struct StateDef {
    LifecycleState state;
    const char*    name;
    bool           accepts_user_msgs   : 1;
    bool           accepts_system_msgs : 1;
    uint8_t        num_transitions     : 3;
    LifecycleState transitions[7];
};

constexpr StateDef kStateMachine[] = {
    { LifecycleState::kStarting,   "starting",   {false, true},  1, {LifecycleState::kActive, LifecycleState::kFailed}},
    { LifecycleState::kActive,     "active",     {true,  true},  3, {LifecycleState::kDraining, LifecycleState::kStopping, LifecycleState::kFailed}},
    { LifecycleState::kDraining,   "draining",   {false, true},  2, {LifecycleState::kStopping, LifecycleState::kFailed}},
    { LifecycleState::kStopping,   "stopping",   {false, true},  2, {LifecycleState::kStopped, LifecycleState::kFailed}},
    { LifecycleState::kStopped,    "stopped",    {false, false}, 1, {LifecycleState::kStarting}},
    { LifecycleState::kFailed,     "failed",     {false, true},  3, {LifecycleState::kStarting, LifecycleState::kStopped, LifecycleState::kRecovering}},
    { LifecycleState::kRecovering, "recovering", {false, true},  2, {LifecycleState::kActive, LifecycleState::kFailed}},
};

static_assert(sizeof(kStateMachine) / sizeof(StateDef) == 7,
              "kStateMachine must have exactly 7 entries");

} // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/actor/lifecycle_state.hpp
git commit -m "feat(lifecycle): add LifecycleState enum and constexpr StateDef table"
```

---

### Task 2: Create lifecycle_actor.hpp — LifecycleActor mixin

**Files:**
- Create: `include/hpactor/actor/lifecycle_actor.hpp`

- [ ] **Step 1: Write the header file**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

#include <hpactor/actor/lifecycle_state.hpp>
#include <hpactor/types/types.hpp>  // complete error type needed for value members

namespace hpactor {

class LifecycleActor {
public:
    LifecycleActor()
        : state_(static_cast<uint8_t>(LifecycleState::kStarting))
        , incarnation_(0) {}
    virtual ~LifecycleActor() = default;

    // ── State queries ──────────────────────────────────
    LifecycleState state() const noexcept {
        return static_cast<LifecycleState>(state_.load(std::memory_order_acquire));
    }
    bool accepts_user_msgs() const noexcept {
        return kStateMachine[static_cast<int>(state())].accepts_user_msgs;
    }
    bool accepts_system_msgs() const noexcept {
        return kStateMachine[static_cast<int>(state())].accepts_system_msgs;
    }
    const char* state_string() const noexcept {
        return kStateMachine[static_cast<int>(state())].name;
    }
    uint64_t incarnation() const noexcept {
        return incarnation_.load(std::memory_order_acquire);
    }
    void bump_incarnation() {
        incarnation_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Failure reason ─────────────────────────────────
    // Set before transition(kFailed) so on_fail() receives the real error.
    void set_failure_reason(error err) {
        failure_reason_ = err;
    }
    error failure_reason() const {
        return failure_reason_;
    }

    // ── Transition ─────────────────────────────────────
    // Validates that `to` is a legal transition from the current state,
    // performs a CAS, and invokes the corresponding virtual hook.
    // Returns false if the transition is illegal or CAS fails.
    bool transition(LifecycleState to);

    // ── Virtual hooks (default = no-op) ────────────────
    virtual void on_start()    {}
    virtual void on_drain()    {}
    virtual void on_stop()     {}
    virtual void on_deactivate() {}
    virtual void on_fail(error err);
    virtual void on_recover()  {}
    virtual void on_restart()  {}

    // NOTE: LifecycleActor does NOT override as_lifecycle().
    // Each lifecycle actor class must explicitly override
    // AbstractActor::as_lifecycle() to return this. Example:
    //   LifecycleActor* as_lifecycle() override { return this; }
    // This is required because LifecycleActor does not inherit from
    // AbstractActor, so it cannot override its virtual method directly.

protected:
    std::atomic<uint8_t> state_;
    std::atomic<uint64_t> incarnation_;
    error failure_reason_{0};
};

} // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/actor/lifecycle_actor.hpp
git commit -m "feat(lifecycle): add LifecycleActor mixin class"
```

---

### Task 3: Create lifecycle_actor.cpp — transition() implementation

**Files:**
- Create: `src/actor/lifecycle_actor.cpp`

- [ ] **Step 1: Write the implementation file**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

void LifecycleActor::on_fail(error /*err*/) {
    // Default no-op; subclasses override for failure-specific cleanup
}

bool LifecycleActor::transition(LifecycleState to) {
    LifecycleState from = state();

    // Validate that `to` is in this state's transition list
    const auto& def = kStateMachine[static_cast<int>(from)];
    bool legal = false;
    for (uint8_t i = 0; i < def.num_transitions; ++i) {
        if (def.transitions[i] == to) {
            legal = true;
            break;
        }
    }
    if (!legal) return false;

    // CAS: only change if still in `from`
    uint8_t expected = static_cast<uint8_t>(from);
    uint8_t desired = static_cast<uint8_t>(to);
    if (!state_.compare_exchange_strong(expected, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        return false;
    }

    // Post-transition: invoke the hook for this transition.
    // on_fail() uses the stored failure_reason_ (set by caller before transition).
    if (to == LifecycleState::kActive && (from == LifecycleState::kStarting || from == LifecycleState::kRecovering)) {
        on_start();
    } else if (to == LifecycleState::kDraining) {
        on_drain();
    } else if (to == LifecycleState::kStopping) {
        on_stop();
    } else if (to == LifecycleState::kStopped) {
        on_deactivate();
    } else if (to == LifecycleState::kFailed) {
        on_fail(failure_reason_);
    } else if (to == LifecycleState::kStarting && (from == LifecycleState::kFailed || from == LifecycleState::kStopped)) {
        on_restart();
    } else if (to == LifecycleState::kRecovering) {
        on_recover();
    }

    return true;
}

} // namespace hpactor
```

- [ ] **Step 2: Add to CMakeLists.txt and verify compilation**

In the top-level `CMakeLists.txt`, find the `hpactor_lib` source file list. Insert `src/actor/lifecycle_actor.cpp` after the existing `src/actor/` entries. The source entries are alphabetically grouped under `src/actor/` — add after `src/actor/http_gateway_actor.cpp` (line ~187).

```bash
# Verify the file is added correctly:
grep "lifecycle_actor" CMakeLists.txt
```

- [ ] **Step 3: Commit**

```bash
git add src/actor/lifecycle_actor.cpp CMakeLists.txt
git commit -m "feat(lifecycle): implement LifecycleActor::transition() with hook dispatch"
```

---

### Task 4: Modify AbstractActor — add as_lifecycle() virtual method

**Files:**
- Modify: `include/hpactor/actor/abstract_actor.hpp`

- [ ] **Step 1: Add forward declaration and virtual method**

In `abstract_actor.hpp`, after existing forward declarations (around line 35), add:

```cpp
class LifecycleActor;
```

In the public section of `AbstractActor`, after `is_event_based_actor()` (around line 93), add:

```cpp
// Lifecycle query — RTTI-free downcast to LifecycleActor mixin.
// Returns nullptr for actors that don't opt into lifecycle management.
virtual LifecycleActor* as_lifecycle() { return nullptr; }
virtual const LifecycleActor* as_lifecycle() const { return nullptr; }
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/actor/abstract_actor.hpp
git commit -m "feat(lifecycle): add as_lifecycle() virtual method to AbstractActor"
```

---

### Task 5: Update to_metadata() to expose lifecycle state

**Files:**
- Modify: `src/actor/abstract_actor.cpp`

- [ ] **Step 1: Add include and update to_metadata()**

Add at top of file:
```cpp
#include <hpactor/actor/lifecycle_actor.hpp>
```

Replace the `to_metadata()` implementation (lines 88-95) with:

```cpp
cli::ActorMeta AbstractActor::to_metadata() const {
    cli::ActorMeta m;
    m.actor_id = id().value();
    m.actor_type = std::string(type_name());
    if (auto* lc = as_lifecycle()) {
        m.state = lc->state_string();
    } else {
        m.state = "unknown";
    }
    m.incarnation = address().incarnation;
    return m;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/actor/abstract_actor.cpp
git commit -m "feat(lifecycle): update to_metadata() to read lifecycle state"
```

---

### Task 6: Add lifecycle message gate in EventBasedActor::receive()

**Files:**
- Modify: `src/actor/event_based_actor.cpp`

- [ ] **Step 1: Add include and lifecycle gate**

Add include at top:
```cpp
#include <hpactor/actor/lifecycle_actor.hpp>
```

After the system message interception block (after line 161 `// -- End system message interception --`) and before `if (!handlers_initialized_)` (line 164), insert the lifecycle gate. The system messages already return early, so the gate only applies to messages that fall through (user messages + CLI messages + DownMsg).

Insert after line 161:

```cpp
    // -- Lifecycle message gate --
    // System messages and CLI/metrics messages (TypeTag < 0x1000) always pass.
    // User messages (TypeTag >= 0x1000) are only accepted in ACTIVE state.
    if (static_cast<uint16_t>(msg.type_id()) >= 0x1000) {
        if (auto* lc = as_lifecycle()) {
            if (!lc->accepts_user_msgs()) {
                // Reject user message — not accepting in this state
                if (metrics_ring_buffer_) [[unlikely]] {
                    metrics::MetricEvent evt{};
                    evt.actor_id = id();
                    evt.event_type = metrics::MetricEventType::kMessageRejected;
                    evt.code = static_cast<uint8_t>(lc->state());
                    metrics_ring_buffer_->try_push(evt);
                }
                return;
            }
        }
    }
    // -- End lifecycle message gate --
```

Wait — `kMessageRejected` doesn't exist yet. We'll add it in Task 8. For now, just skip the metrics emission or use a simpler gate. To keep this task self-contained, let's just do the gate without the metrics event emission, and add the metrics emission in Task 8.

Actually, let's keep this simpler. The gate itself is the important part:

```cpp
    // -- Lifecycle message gate --
    // User messages (TypeTag >= 0x1000) are only accepted in ACTIVE state.
    // System messages (TypeTag < 0x1000) always pass through.
    if (static_cast<uint16_t>(msg.type_id()) >= 0x1000) {
        if (auto* lc = as_lifecycle()) {
            if (!lc->accepts_user_msgs()) {
                return;
            }
        }
    }
    // -- End lifecycle message gate --
```

We'll add the metrics emission for rejected messages as part of Task 8.

- [ ] **Step 2: Commit**

```bash
git add src/actor/event_based_actor.cpp
git commit -m "feat(lifecycle): add message gate in EventBasedActor::receive()"
```

---

### Task 7: Update KillRequest handler for lifecycle

**Files:**
- Modify: `src/actor/event_based_actor.cpp`

- [ ] **Step 1: Update the KillRequest handler**

In the existing KillRequest handler (lines 213-231), replace the `set_exit_reason(0)` line with lifecycle transitions:

Replace lines 228-230:
```cpp
            // Schedule termination with normal exit code
            set_exit_reason(0);
            return;
```

With:
```cpp
            // Drive lifecycle state machine for graceful stop
            if (auto* lc = as_lifecycle()) {
                lc->transition(LifecycleState::kStopping);
                lc->transition(LifecycleState::kStopped);
            }
            set_exit_reason(0);
            return;
```

- [ ] **Step 2: Commit**

```bash
git add src/actor/event_based_actor.cpp
git commit -m "feat(lifecycle): update KillRequest handler to drive lifecycle transitions"
```

---

### Task 8: Add metric event types for lifecycle

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp`

- [ ] **Step 1: Add kLifecycleTransition and kMessageRejected event types**

After `kDeadLetterLost = 14` (line 37), add:

```cpp
    kLifecycleTransition = 15,
    kMessageRejected = 16,
```

**Note:** `kMailboxRejected = 10` already exists but is for mailbox-level rejection. `kMessageRejected = 16` is for lifecycle-gated rejection (different rejection domain).

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp
git commit -m "feat(lifecycle): add kLifecycleTransition and kMessageRejected metric event types"
```

---

### Task 9: Update metrics aggregator for lifecycle events

**Files:**
- Modify: `src/metrics/metrics_aggregator.cpp`

- [ ] **Step 1: Add lifecycle transition case to aggregator**

Read the aggregator's event dispatch switch to find the right insertion point:

```bash
grep -n "case MetricEventType::k" src/metrics/metrics_aggregator.cpp
```

Add after the last existing case:
```cpp
        case MetricEventType::kLifecycleTransition:
            // from_state in e.code, to_state in e.aux
            break;
        case MetricEventType::kMessageRejected:
            // rejected-by state in e.code
            break;
```

`MetricEvent` has `code` and `aux` fields (not `value_lo`). This is a minimal stub that compiles — full aggregation logic is deferred.

- [ ] **Step 2: Commit**

```bash
git add src/metrics/metrics_aggregator.cpp
git commit -m "feat(lifecycle): add lifecycle transition aggregation in metrics"
```

---

### Task 10: Wire lifecycle in ActorSystem::spawn()

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp` (template method, line ~514)

- [ ] **Step 1: Add include in actor_system.hpp**

In `include/hpactor/core/actor_system.hpp`, add after existing includes:
```cpp
#include <hpactor/actor/lifecycle_actor.hpp>
```

- [ ] **Step 2: Add lifecycle transition after on_activate()**

In the `spawn()` template method (line ~514, after `actor->on_activate()`), add:

```cpp
    // Transition lifecycle to ACTIVE if actor has lifecycle management
    if (auto* lc = actor->as_lifecycle()) {
        lc->transition(LifecycleState::kActive);
    }
```

Note: `src/actor/actor_system.cpp` does NOT need a new include — the lifecycle wiring is in the template defined in the header.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/core/actor_system.hpp
git commit -m "feat(lifecycle): wire lifecycle STARTING→ACTIVE in ActorSystem::spawn()"
```

---

### Task 11: Wire lifecycle in supervision restart

**Files:**
- Modify: `src/supervision/supervision.cpp`

- [ ] **Step 1: Add include**

At top of file:
```cpp
#include <hpactor/actor/lifecycle_actor.hpp>
```

- [ ] **Step 2: Update SupervisorActor::restart_child()**

First, update the method signature to accept the failure reason. In `supervision.hpp` find `restart_child(ActorId child_id)` and change to `restart_child(ActorId child_id, const error& reason)`. In `supervision.cpp`, update the definition at line 91.

In `handle_child_down()`, update the call site at line 77 from `restart_child(child_id)` to `restart_child(child_id, reason)`.

Then, in `restart_child()` (after `++count;` at line 110, before metrics at line 112), add:

```cpp
    // Drive lifecycle for the failing child
    if (auto actor = system().get_actor(child_id)) {
        if (auto* lc = actor.get()->as_lifecycle()) {
            lc->set_failure_reason(reason);
            lc->transition(LifecycleState::kFailed);
            lc->bump_incarnation();
            lc->transition(LifecycleState::kStarting);
        }
    }
```

Note: The actual respawn is subclass-specific. The base `SupervisorActor::restart_child()` manages the counter and drives the lifecycle. Subclasses override to provide actual respawn logic.

Also update `restart_all_children()` — for now, it passes `error(0)` since it has no specific failure context.

- [ ] **Step 3: Commit**

```bash
git add src/supervision/supervision.cpp
git commit -m "feat(lifecycle): drive FAILED→STARTING→ACTIVE lifecycle in supervisor restart"
```

---

### Task 12: Create test_lifecycle_state.cpp — state machine unit tests

**Files:**
- Create: `tests/actor/test_lifecycle_state.cpp`

- [ ] **Step 1: Write the test file**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle_state.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <iostream>

using namespace hpactor;

// Test harness: LifecycleActor with tracking
class TestLifecycleActor : public LifecycleActor {
public:
    int start_calls = 0;
    int drain_calls = 0;
    int stop_calls = 0;
    int deactivate_calls = 0;
    int fail_calls = 0;
    int recover_calls = 0;
    int restart_calls = 0;

    void on_start() override    { start_calls++; }
    void on_drain() override    { drain_calls++; }
    void on_stop() override     { stop_calls++; }
    void on_deactivate() override { deactivate_calls++; }
    void on_fail(error) override { fail_calls++; }
    void on_recover() override  { recover_calls++; }
    void on_restart() override  { restart_calls++; }
};

#define TEST(name) static void name()
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << '\n'; std::abort(); } } while(0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

// ── Test 1: Default state is STARTING ───────────────────
TEST(test_default_state_is_starting) {
    TestLifecycleActor a;
    CHECK_EQ(a.state(), LifecycleState::kStarting);
    CHECK_EQ(std::string(a.state_string()), "starting");
    std::cout << "PASS: test_default_state_is_starting\n";
}

// ── Test 2: Legal transition STARTING→ACTIVE ────────────
TEST(test_starting_to_active) {
    TestLifecycleActor a;
    bool ok = a.transition(LifecycleState::kActive);
    CHECK(ok);
    CHECK_EQ(a.state(), LifecycleState::kActive);
    CHECK_EQ(a.start_calls, 1);
    std::cout << "PASS: test_starting_to_active\n";
}

// ── Test 3: Illegal transition ACTIVE→STARTING ──────────
TEST(test_active_to_starting_illegal) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kStarting);
    CHECK(!ok);
    CHECK_EQ(a.state(), LifecycleState::kActive);
    std::cout << "PASS: test_active_to_starting_illegal\n";
}

// ── Test 4: Illegal transition ACTIVE→RECOVERING ────────
TEST(test_active_to_recovering_illegal) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kRecovering);
    CHECK(!ok);
    CHECK_EQ(a.state(), LifecycleState::kActive);
    std::cout << "PASS: test_active_to_recovering_illegal\n";
}

// ── Test 5: Full happy path ─────────────────────────────
TEST(test_full_happy_path) {
    TestLifecycleActor a;
    CHECK(a.transition(LifecycleState::kActive));    // STARTING→ACTIVE
    CHECK_EQ(a.start_calls, 1);
    CHECK(a.transition(LifecycleState::kDraining));  // ACTIVE→DRAINING
    CHECK_EQ(a.drain_calls, 1);
    CHECK(a.transition(LifecycleState::kStopping));  // DRAINING→STOPPING
    CHECK_EQ(a.stop_calls, 1);
    CHECK(a.transition(LifecycleState::kStopped));   // STOPPING→STOPPED
    CHECK_EQ(a.deactivate_calls, 1);
    CHECK_EQ(a.state(), LifecycleState::kStopped);
    std::cout << "PASS: test_full_happy_path\n";
}

// ── Test 6: Failure + restart path ──────────────────────
TEST(test_failure_restart_path) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    CHECK(a.transition(LifecycleState::kFailed));     // ACTIVE→FAILED
    CHECK_EQ(a.fail_calls, 1);
    CHECK_EQ(a.state(), LifecycleState::kFailed);
    a.bump_incarnation();
    CHECK(a.transition(LifecycleState::kStarting));   // FAILED→STARTING
    CHECK_EQ(a.restart_calls, 1);
    CHECK(a.transition(LifecycleState::kActive));     // STARTING→ACTIVE
    CHECK_EQ(a.start_calls, 1);
    CHECK_EQ(a.restart_calls, 1);
    std::cout << "PASS: test_failure_restart_path\n";
}

// ── Test 7: Recovery path ───────────────────────────────
TEST(test_recovery_path) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kFailed);
    CHECK(a.transition(LifecycleState::kRecovering)); // FAILED→RECOVERING
    CHECK_EQ(a.recover_calls, 1);
    CHECK(a.transition(LifecycleState::kActive));     // RECOVERING→ACTIVE
    CHECK_EQ(a.start_calls, 1); // on_start reused
    std::cout << "PASS: test_recovery_path\n";
}

// ── Test 8: state_string() for each state ───────────────
TEST(test_state_string) {
    TestLifecycleActor a;
    CHECK_EQ(std::string(a.state_string()), "starting");
    a.transition(LifecycleState::kActive);
    CHECK_EQ(std::string(a.state_string()), "active");
    a.transition(LifecycleState::kDraining);
    CHECK_EQ(std::string(a.state_string()), "draining");
    a.transition(LifecycleState::kStopping);
    CHECK_EQ(std::string(a.state_string()), "stopping");
    a.transition(LifecycleState::kStopped);
    CHECK_EQ(std::string(a.state_string()), "stopped");
    std::cout << "PASS: test_state_string\n";
}

// ── Test 9: accepts_user_msgs() true only in ACTIVE ─────
TEST(test_accepts_user_msgs) {
    TestLifecycleActor a;
    // STARTING: false
    CHECK(!a.accepts_user_msgs());
    a.transition(LifecycleState::kActive);
    // ACTIVE: true
    CHECK(a.accepts_user_msgs());
    a.transition(LifecycleState::kDraining);
    // DRAINING: false
    CHECK(!a.accepts_user_msgs());
    std::cout << "PASS: test_accepts_user_msgs\n";
}

// ── Test 10: accepts_system_msgs() false only in STOPPED ─
TEST(test_accepts_system_msgs) {
    TestLifecycleActor a;
    CHECK(a.accepts_system_msgs()); // STARTING: true
    a.transition(LifecycleState::kActive);
    CHECK(a.accepts_system_msgs()); // ACTIVE: true
    a.transition(LifecycleState::kDraining);
    CHECK(a.accepts_system_msgs()); // DRAINING: true
    a.transition(LifecycleState::kStopping);
    CHECK(a.accepts_system_msgs()); // STOPPING: true
    a.transition(LifecycleState::kStopped);
    CHECK(!a.accepts_system_msgs()); // STOPPED: false
    std::cout << "PASS: test_accepts_system_msgs\n";
}

// ── Test 11: Transition invokes correct hook ────────────
TEST(test_transition_invokes_correct_hook) {
    TestLifecycleActor a;
    // STARTING→ACTIVE: on_start
    CHECK(a.transition(LifecycleState::kActive));
    CHECK_EQ(a.start_calls, 1);
    CHECK_EQ(a.drain_calls, 0);
    // ACTIVE→DRAINING: on_drain
    CHECK(a.transition(LifecycleState::kDraining));
    CHECK_EQ(a.drain_calls, 1);
    // DRAINING→STOPPING: on_stop
    CHECK(a.transition(LifecycleState::kStopping));
    CHECK_EQ(a.stop_calls, 1);
    // STOPPING→STOPPED: on_deactivate
    CHECK(a.transition(LifecycleState::kStopped));
    CHECK_EQ(a.deactivate_calls, 1);
    // Verify no spillover to other hooks
    CHECK_EQ(a.fail_calls, 0);
    CHECK_EQ(a.recover_calls, 0);
    CHECK_EQ(a.restart_calls, 0);
    std::cout << "PASS: test_transition_invokes_correct_hook\n";
}

// ── Test 12: Incarnation bumps ──────────────────────────
TEST(test_incarnation_bumps) {
    TestLifecycleActor a;
    CHECK_EQ(a.incarnation(), 0);
    a.bump_incarnation();
    CHECK_EQ(a.incarnation(), 1);
    a.bump_incarnation();
    CHECK_EQ(a.incarnation(), 2);
    std::cout << "PASS: test_incarnation_bumps\n";
}

int main() {
    test_default_state_is_starting();
    test_starting_to_active();
    test_active_to_starting_illegal();
    test_active_to_recovering_illegal();
    test_full_happy_path();
    test_failure_restart_path();
    test_recovery_path();
    test_state_string();
    test_accepts_user_msgs();
    test_accepts_system_msgs();
    test_transition_invokes_correct_hook();
    test_incarnation_bumps();
    std::cout << "\nAll 12 lifecycle state tests passed.\n";
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add tests/actor/test_lifecycle_state.cpp
git commit -m "test(lifecycle): add 12 state machine unit tests"
```

---

### Task 13: Create test_lifecycle_actor.cpp — integration tests

**Files:**
- Create: `tests/actor/test_lifecycle_actor.cpp`

- [ ] **Step 1: Write the integration test file**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>
#include <iostream>
#include <thread>

using namespace hpactor;

#define TEST(name) static void name()
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << '\n'; std::abort(); } } while(0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

// ── Test 1: Actor without LifecycleActor returns nullptr ─
TEST(test_no_lifecycle_returns_null) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();
    CHECK(actor.get()->as_lifecycle() == nullptr);
    std::cout << "PASS: test_no_lifecycle_returns_null\n";
}

// ── Test 2: Lifecycle actor spawns as ACTIVE ────────────
class SimpleLifecycleActor : public EventBasedActor, public LifecycleActor {
public:
    SimpleLifecycleActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    // REQUIRED: override AbstractActor::as_lifecycle() to return this.
    // LifecycleActor does NOT inherit from AbstractActor, so the override
    // must appear in the class that inherits from both.
    LifecycleActor* as_lifecycle() override { return this; }
    const LifecycleActor* as_lifecycle() const override { return this; }
};

TEST(test_lifecycle_actor_spawns_active) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<SimpleLifecycleActor>();
    auto* lc = actor.get()->as_lifecycle();
    CHECK(lc != nullptr);
    // After spawn(), the lifecycle should be ACTIVE
    CHECK_EQ(lc->state(), LifecycleState::kActive);
    std::cout << "PASS: test_lifecycle_actor_spawns_active\n";
}

// ── Test 5: to_metadata() reports lifecycle state ────────
TEST(test_to_metadata_reports_lifecycle_state) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<SimpleLifecycleActor>();
    auto meta = actor.get()->to_metadata();
    CHECK_EQ(meta.state, "active");
    std::cout << "PASS: test_to_metadata_reports_lifecycle_state\n";
}

// ── Test 6: Default actor to_metadata() says "unknown" ──
TEST(test_default_actor_to_metadata_unknown) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();
    auto meta = actor.get()->to_metadata();
    CHECK_EQ(meta.state, "unknown");
    std::cout << "PASS: test_default_actor_to_metadata_unknown\n";
}

int main() {
    test_no_lifecycle_returns_null();
    test_lifecycle_actor_spawns_active();
    test_to_metadata_reports_lifecycle_state();
    test_default_actor_to_metadata_unknown();
    // Tests 3,4,7,8 require actor message passing — deferred to
    // when ActorSystem is fully wired for lifecycle gating
    std::cout << "\nAll lifecycle actor integration tests passed.\n";
    return 0;
}
```

**Note:** Tests 3 (user message rejected), 4 (system messages accepted), 7 (KillRequest), and 8 (supervisor restart) require the full ActorSystem with message passing — they'll be filled in once lifecycle gating is fully integrated. This preserves the test file structure from the design while shipping the tests we can verify now.

- [ ] **Step 2: Commit**

```bash
git add tests/actor/test_lifecycle_actor.cpp
git commit -m "test(lifecycle): add integration tests for lifecycle actor"
```

---

### Task 14: Add test targets to tests/CMakeLists.txt

**Files:**
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add new test executables**

After the existing actor test section (after `test_typed_actor` block), add:

```cmake
add_executable(test_lifecycle_state actor/test_lifecycle_state.cpp)
target_link_libraries(test_lifecycle_state hpactor)
add_test(NAME test_lifecycle_state COMMAND test_lifecycle_state)

add_executable(test_lifecycle_actor actor/test_lifecycle_actor.cpp)
target_link_libraries(test_lifecycle_actor hpactor)
add_test(NAME test_lifecycle_actor COMMAND test_lifecycle_actor)
```

- [ ] **Step 2: Commit**

```bash
git add tests/CMakeLists.txt
git commit -m "build: add test_lifecycle_state and test_lifecycle_actor targets"
```

---

### Task 15: Build and run all tests

**Files:**
- None (verification only)

- [ ] **Step 1: Configure**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

- [ ] **Step 2: Build**

```bash
ninja -C build
```

Expected: Build succeeds with zero errors.

- [ ] **Step 3: Run new lifecycle tests**

```bash
./build/tests/test_lifecycle_state
./build/tests/test_lifecycle_actor
```

Expected: All tests pass.

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure
```

Expected: All existing 126 tests continue to pass. No regressions.

- [ ] **Step 5: Commit any fixes if needed**

```bash
git add -A
git commit -m "fix(lifecycle): build fixes from verification"
```

---

### Task 16: Update CLAUDE_MEMORY.md

**Files:**
- Modify: `CLAUDE_MEMORY.md`

- [ ] **Step 1: Record the completed feature**

Add lifecycle entry to the project memory file summarizing what was built.

- [ ] **Step 2: Commit**

```bash
git add CLAUDE_MEMORY.md
git commit -m "docs: update project memory for actor lifecycle feature"
```
