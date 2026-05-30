# MBX-005 Priority Mailbox Lanes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract `MultiLaneQueue<T>` from `MPSCActorMailbox<T>`, then add system-lane isolation, priority-aware routing, `DropLowestPriority` overflow, per-lane metrics/CLI, and TOML config.

**Architecture:** Phase 0 extracts a standalone `MultiLaneQueue<T>` owning 1 system + N user MPSC queues with priority-ordered consumer drain. Phases 1-5 build features on the clean abstraction. Default `priority_aware = false` preserves existing single-lane FIFO.

**Tech Stack:** C++20, header-only templates, lock-free MPSC queues, Google Test, CMake+Ninja, `-j1` builds.

**Design spec:** `docs/architecture/production/priority-mailbox-lanes-design.md`

---

### Task 0: Baseline build and test

- [ ] **Step 1: Configure and build**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && ninja -C build -j1
```

Expected: Build succeeds.

- [ ] **Step 2: Run full test suite**

```bash
ctest --output-on-failure --parallel 8 2>&1 | tail -5
```

Expected: "100% tests passed".

---

### Task 1: Create MultiLaneQueue<T> header

**Files:** Create `include/hpactor/mailbox/multi_lane_queue.hpp`

- [ ] **Step 1: Write `multi_lane_queue.hpp`**

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

#include <hpactor/mailbox/mpsc_mailbox.hpp>
#include <hpactor/mem/memory_config.hpp>

#include <cstdint>
#include <vector>

namespace hpactor::mailbox {

/// Multi-lane MPSC queue container for priority-aware actor mailboxes.
///
/// Owns one system lane + N user lanes. Producers enqueue lock-free into
/// a specific lane. The consumer drains in fixed priority order:
/// system lane first, then user lanes 0..N-1.
///
/// Does NOT own the consumer lock — the caller (MPSCActorMailbox)
/// serializes dequeue/eviction externally. Does NOT know about
/// reservation, pressure, overflow policy, metrics, or scheduler wakeup.
///
/// \tparam T Message type with `std::atomic<T*> mpsc_next` member.
template <typename T>
class MultiLaneQueue {
public:
    static constexpr uint8_t kSystemLaneSentinel = 0xFF;
    static constexpr uint8_t kMaxUserLanes = 8;

    explicit MultiLaneQueue(uint8_t num_user_lanes = 1)
        : user_lanes_(num_user_lanes) {}

    // ── Producer (lock-free, multi-producer safe) ─────────────────

    void enqueue(T* node, uint8_t lane_idx) noexcept {
        if (lane_idx == kSystemLaneSentinel) {
            system_lane_.enqueue(node);
        } else {
            user_lanes_[lane_idx].enqueue(node);
        }
    }

    // ── Consumer (NOT internally locked — caller serializes) ──────

    /// Dequeue in priority order: system lane, then user lanes 0..N-1.
    /// Returns nullptr if all lanes are empty.
    T* dequeue() noexcept {
        T* node = system_lane_.dequeue();
        if (node) return node;
        for (auto& lane : user_lanes_) {
            node = lane.dequeue();
            if (node) return node;
        }
        return nullptr;
    }

    /// Drop one message from the lowest-priority non-empty user lane.
    /// Does NOT touch the system lane.
    /// \return The dropped node (caller handles reservation release
    ///         and deferred destruction), or nullptr if all empty.
    T* try_drop_from_lowest_user_lane() noexcept {
        for (int i = static_cast<int>(user_lanes_.size()) - 1; i >= 0; --i) {
            T* node = user_lanes_[static_cast<size_t>(i)].dequeue();
            if (node) return node;
        }
        return nullptr;
    }

    // ── Pending free (deferred destructor) ────────────────────────

    void set_pending_free(T* node) noexcept {
        if (pending_free_) {
            pending_free_->~T();
            mem::deallocate(pending_free_);
        }
        pending_free_ = node;
    }

    T* release_pending_free() noexcept {
        T* p = pending_free_;
        pending_free_ = nullptr;
        return p;
    }

    // ── Query ─────────────────────────────────────────────────────

    bool empty() const noexcept {
        if (!system_lane_.empty()) return false;
        for (const auto& lane : user_lanes_)
            if (!lane.empty()) return false;
        return true;
    }

    int64_t total_depth() const noexcept {
        int64_t d = system_lane_.count();
        for (const auto& lane : user_lanes_) d += lane.count();
        return d;
    }

    int64_t lane_depth(uint8_t lane_idx) const noexcept {
        if (lane_idx == kSystemLaneSentinel) return system_lane_.count();
        return user_lanes_[lane_idx].count();
    }

    uint8_t num_user_lanes() const noexcept {
        return static_cast<uint8_t>(user_lanes_.size());
    }

    void set_num_user_lanes(uint8_t n) { user_lanes_.resize(n); }

    // ── Test support ──────────────────────────────────────────────

    void inject_for_test(T* node, uint8_t lane_idx) noexcept {
        enqueue(node, lane_idx);
    }

    void reset() noexcept {
        while (dequeue() != nullptr) {}
        if (pending_free_) {
            pending_free_->~T();
            mem::deallocate(pending_free_);
            pending_free_ = nullptr;
        }
    }

private:
    MPSCMailbox<T> system_lane_;
    std::vector<MPSCMailbox<T>> user_lanes_;
    T* pending_free_{nullptr};
};

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Verify header compiles in isolation**

```bash
echo '#include <hpactor/mailbox/multi_lane_queue.hpp>
int main() {
    struct N { int v; std::atomic<N*> mpsc_next{nullptr}; };
    hpactor::mailbox::MultiLaneQueue<N> q(4);
    return 0;
}' > /tmp/test_mlq.cpp
g++ -std=c++20 -I include -fsyntax-only /tmp/test_mlq.cpp && echo "OK"
```

Expected: "OK"

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/mailbox/multi_lane_queue.hpp
git commit -m "refactor(mailbox): add MultiLaneQueue<T> header

Pure data structure — owns 1 system + N user MPSC queues with
priority-ordered consumer drain. Not yet wired into MPSCActorMailbox.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 2: Replace MPSCMailbox member with MultiLaneQueue in MPSCActorMailbox

**Files:** Modify `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

Every `mailbox_.xxx()` call is replaced with the equivalent `lanes_.xxx()`.
No behavior change. This is a pure mechanical refactor.

- [ ] **Step 1: Add include and replace member declaration**

Add `#include <hpactor/mailbox/multi_lane_queue.hpp>` near the other mailbox includes.

Replace:
```cpp
MPSCMailbox<T> mailbox_;
```
With:
```cpp
MultiLaneQueue<T> lanes_{1};
```

- [ ] **Step 2: Replace `mailbox_` uses — producer side**

In `enqueue_reserved()` (~line 208), change:
```cpp
// OLD:
mailbox_.enqueue(node);
// NEW:
lanes_.enqueue(node, 0);
```

In `inject_for_test()` (~line 319), change:
```cpp
// OLD:
mailbox_.enqueue(node);
// NEW:
lanes_.inject_for_test(node, 0);
```

- [ ] **Step 3: Replace `mailbox_` uses — consumer side**

In `dequeue()` (~line 246), change:
```cpp
// OLD:
T* node = mailbox_.dequeue();
// NEW:
T* node = lanes_.dequeue();
```

- [ ] **Step 4: Replace `mailbox_` uses — queries**

Search for every remaining `mailbox_.` and replace:

| `mailbox_.empty()` | `lanes_.empty()` |
| `mailbox_.count()` | `lanes_.total_depth()` |

These appear in: `try_push` OverflowContext construction (~line 128),
the log warning depth check (~line 215), `make_result` (~line 466),
`pressure_ratio()` (~line 428), `update_max_depth()` (~line 450),
`snapshot()` (~line 326).

- [ ] **Step 5: Rewrite `drop_one_oldest()` to use the new API**

The current `drop_one_oldest()` (~lines 362-401) does:
1. Lock consumer
2. `mailbox_.dequeue()` → get node
3. Release reservation (system or regular)
4. Increment drop counter, update pressure, emit metric
5. Check empty, unlock
6. Manage `pending_free_`

Replace with delegating to `lanes_.try_drop_from_lowest_user_lane()` for step 2
and `lanes_.set_pending_free(node)` for step 6:

```cpp
bool drop_one_oldest() noexcept {
    FAULT_INJECT("hpactor.mailbox.drop_oldest.fail") {
        return false;
    }
    lock_consumer();
    T* node = lanes_.try_drop_from_lowest_user_lane();
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
    if (lanes_.empty()) {
        mailbox_was_empty_.store(true, std::memory_order_release);
    }
    unlock_consumer();
    lanes_.set_pending_free(node);
    return true;
}
```

- [ ] **Step 6: Update destructor**

Replace the `pending_free_` destructor logic (~lines 57-61):
```cpp
// OLD:
~MPSCActorMailbox() {
    if (pending_free_) {
        pending_free_->~T();
        mem::deallocate(pending_free_);
    }
}
// NEW:
~MPSCActorMailbox() {
    T* p = lanes_.release_pending_free();
    if (p) {
        p->~T();
        mem::deallocate(p);
    }
}
```

Remove member: `T* pending_free_{nullptr};`

- [ ] **Step 7: Build and run full test suite**

```bash
ninja -C build -j1 && ctest --output-on-failure --parallel 8 2>&1 | tail -5
```

Expected: All tests pass. **If any test fails, fix before proceeding.**

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp
git commit -m "refactor(mailbox): replace MPSCMailbox with MultiLaneQueue

Pure mechanical refactor. All mailbox_ accesses replaced with equivalent
lanes_ calls. pending_free_ management moved into MultiLaneQueue.
drop_one_oldest delegates to try_drop_from_lowest_user_lane.
All existing tests pass unchanged with single user lane.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 3: Route system messages to dedicated system lane

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Modify: `include/hpactor/mailbox/detail/reservation_manager.hpp`
- Modify: `tests/unit/mailbox/test_reservation_manager.cpp`

- [ ] **Step 1: Add `route_lane()` helper to MPSCActorMailbox**

Add private method:

```cpp
uint8_t route_lane(const MailboxEnvelopeMeta& meta) const noexcept {
    if (is_system_message(meta.type_tag))
        return MultiLaneQueue<T>::kSystemLaneSentinel;
    return 0;
}
```

- [ ] **Step 2: Add system-message fast path at top of `try_push()`**

Insert after the `estimated_bytes` normalization and before the reservation
attempt. When the message is a system message, check system lane capacity
and enqueue directly (no user-lane reservation needed):

```cpp
if (is_system_message(meta.type_tag)) {
    if (static_cast<uint32_t>(
            lanes_.lane_depth(MultiLaneQueue<T>::kSystemLaneSentinel))
        >= config_.protected_system_messages) {
        update_pressure_state(/*hard_failure=*/true);
        total_rejected_.fetch_add(1, std::memory_order_relaxed);
        EnqueueResult r;
        r.code = EnqueueResultCode::Rejected;
        r.target = actor_id_;
        r.depth = static_cast<uint32_t>(lanes_.total_depth());
        r.capacity = config_.capacity.max_messages;
        r.bytes = reservation_.queued_bytes();
        r.byte_capacity = config_.capacity.max_bytes;
        r.pressure_ratio = pressure_ratio();
        r.pressure_state = pressure_state_.current_state();
        return r;
    }
    void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
    auto* node = new (raw) T(std::move(msg));
    enqueue_reserved(node, meta, MultiLaneQueue<T>::kSystemLaneSentinel);
    return make_result(pressure_state_.code_after_accept());
}
```

- [ ] **Step 3: Remove old system-reserve bypass**

Delete the block inside the `if (reserve_result != Reserved)` branch that
does `try_reserve_system` (lines ~108-166 in current code: the
`bool sys_reserved = false; if (is_system_message(...)) sys_reserved =
reservation_.try_reserve_system(...); if (!sys_reserved) { ... }` block).

Now the overflow handling path only applies to user messages (system messages
were already handled in step 2).

- [ ] **Step 4: Update `enqueue_reserved` signature to accept lane index**

```cpp
// OLD:
void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta,
                      bool suppress_wakeup = false) noexcept
// NEW:
void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta,
                      uint8_t lane_idx = 0,
                      bool suppress_wakeup = false) noexcept
```

Inside the method, change `lanes_.enqueue(node, 0)` to `lanes_.enqueue(node, lane_idx)`.

Update callers that need a non-zero lane:
- In `try_push` user path: `enqueue_reserved(node, meta, route_lane(meta))`
- In `enqueue(T*)`: `enqueue_reserved(node, meta, 0)` (unchanged)
- In `drain_overflow`: `enqueue_reserved(..., 0, true)` (unchanged)

- [ ] **Step 5: Simplify `dequeue()` reservation release**

Replace the `is_system_message` check in `dequeue()` (~lines 256-265):

```cpp
// OLD:
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

// NEW:
bool is_sys = false;
if constexpr (std::is_same_v<T, TypedMessage>) {
    is_sys = is_system_message(node->type_id());
}
if (!is_sys) {
    reservation_.release(bytes);
}
```

- [ ] **Step 6: Remove system methods from ReservationManager**

Delete from `include/hpactor/mailbox/detail/reservation_manager.hpp`:
- `try_reserve_system(uint64_t bytes, uint32_t limit)` method
- `release_system(uint64_t bytes)` method
- `reserved_system_count()` method
- Member `std::atomic<uint32_t> reserved_system_messages_{0};`

- [ ] **Step 7: Remove system-reserve tests from test_reservation_manager.cpp**

Delete these test cases:
- `SystemReserveBypassesByteBudget`
- `SystemReserveRespectsLimit`
- `ReleaseSystemReturnsCapacity`

- [ ] **Step 8: Build and run full test suite**

```bash
ninja -C build -j1 && ctest --output-on-failure --parallel 8 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp \
        include/hpactor/mailbox/detail/reservation_manager.hpp \
        tests/unit/mailbox/test_reservation_manager.cpp
git commit -m "feat(mailbox): route system messages to dedicated system lane

System messages now use MultiLaneQueue's system lane with dedicated
capacity. Removed try_reserve_system/reserved_system_count from
ReservationManager — lane depth is the authoritative counter.
System lane drained before all user lanes.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 4: Add priority-aware user lane routing

**Files:** Modify `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

- [ ] **Step 1: Update `route_lane()`**

```cpp
uint8_t route_lane(const MailboxEnvelopeMeta& meta) const noexcept {
    if (is_system_message(meta.type_tag))
        return MultiLaneQueue<T>::kSystemLaneSentinel;
    if (!config_.priority_aware)
        return 0;
    return std::min<uint8_t>(meta.priority, lanes_.num_user_lanes() - 1);
}
```

- [ ] **Step 2: Add lane resize to `set_config()`**

At the end of `set_config()`, after the existing validation, add:

```cpp
if (cfg.priority_levels != config_.priority_levels) {
    lanes_.set_num_user_lanes(cfg.priority_levels);
}
```

- [ ] **Step 3: Build and run full test suite**

```bash
ninja -C build -j1 && ctest --output-on-failure --parallel 8 2>&1 | tail -5
```

Expected: All tests pass. (`priority_aware` defaults to false, behavior unchanged.)

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp
git commit -m "feat(mailbox): add priority-aware user lane routing

When priority_aware is enabled, user messages route to lanes by
meta.priority (clamped to num_user_lanes-1). Default (false) routes
all to lane 0 preserving FIFO. set_config() resizes lane array.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 5: Implement DropLowestPriority overflow handler

**Files:**
- Create: `include/hpactor/mailbox/detail/handlers/drop_lowest_priority_handler.hpp`
- Modify: `include/hpactor/mailbox/detail/overflow_context.hpp`
- Modify: `include/hpactor/mailbox/detail/overflow_handler_factory.hpp`
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

The handler uses a callback (`drop_lowest_priority_fn`) in `OverflowContext`,
mirroring the existing `drop_oldest_fn` pattern.

- [ ] **Step 1: Add `drop_lowest_priority_fn` to `OverflowContext`**

In `include/hpactor/mailbox/detail/overflow_context.hpp`, add after the
existing `drop_oldest_fn` field:

```cpp
std::function<bool()> drop_lowest_priority_fn;
```

- [ ] **Step 2: Create `drop_lowest_priority_handler.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// ...license...
#pragma once

#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>

namespace hpactor::mailbox::detail {

template <typename T>
class DropLowestPriorityHandler : public IOverflowHandler<T> {
public:
    EnqueueResult handle(OverflowContext<T>& ctx,
                         ReservationResult reason) noexcept override {
        if (ctx.drop_lowest_priority_fn && ctx.drop_lowest_priority_fn()) {
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
        return OverflowPolicy::DropLowestPriority;
    }
};

} // namespace hpactor::mailbox::detail
```

- [ ] **Step 3: Populate `drop_lowest_priority_fn` in `try_push`**

In `mpsc_actor_mailbox.hpp`, where the `OverflowContext` is aggregate-initialized
(~line 117-130), add the new callback after `drop_oldest_fn` and `dlq`:

```cpp
detail::OverflowContext<T> ctx{
    msg, meta, reservation_, overflow_queue_,
    total_rejected_, total_dropped_, total_dead_letters_,
    metrics_ring_buffer_, config_, actor_id_,
    static_cast<uint32_t>(lanes_.total_depth()),
    reservation_.queued_bytes(),
    [this]() { return drop_one_oldest(); },       // drop_oldest_fn
    nullptr,                                        // dlq
    [this]() { return drop_one_oldest(); }};        // drop_lowest_priority_fn
```

- [ ] **Step 4: Wire into the overflow handler factory**

In `include/hpactor/mailbox/detail/overflow_handler_factory.hpp`:

Add include:
```cpp
#include <hpactor/mailbox/detail/handlers/drop_lowest_priority_handler.hpp>
```

Change the `DropLowestPriority` case:
```cpp
// OLD:
case OverflowPolicy::DropLowestPriority:
case OverflowPolicy::BlockWhenAllowed:
default:
    return std::make_unique<RejectNewestHandler<T>>();

// NEW:
case OverflowPolicy::DropLowestPriority:
    return std::make_unique<DropLowestPriorityHandler<T>>();
case OverflowPolicy::BlockWhenAllowed:
default:
    return std::make_unique<RejectNewestHandler<T>>();
```

- [ ] **Step 5: Build and run tests**

```bash
ninja -C build -j1 && ctest --output-on-failure --parallel 8 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mailbox/detail/handlers/drop_lowest_priority_handler.hpp \
        include/hpactor/mailbox/detail/overflow_context.hpp \
        include/hpactor/mailbox/detail/overflow_handler_factory.hpp \
        include/hpactor/mailbox/mpsc_actor_mailbox.hpp
git commit -m "feat(mailbox): implement DropLowestPriority overflow handler

Evicts from lowest-priority non-empty user lane via callback.
Wired into overflow handler factory. Mirrors DropOldestHandler pattern.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 6: Add per-lane metrics to MboxSnapshot

**Files:**
- Modify: `include/hpactor/cli/cli_types.hpp`
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

- [ ] **Step 1: Add per-lane fields to `MboxSnapshot`**

In `cli_types.hpp`, add after `high_priority_depth` (line 50):

```cpp
uint32_t system_lane_depth = 0;
uint32_t lane_depths[8] = {};
uint8_t num_user_lanes = 1;
```

- [ ] **Step 2: Populate from `lanes_` in `snapshot()`**

In `mpsc_actor_mailbox.hpp`, in `snapshot()`, replace `s.high_priority_depth = 0`:

```cpp
s.system_lane_depth = static_cast<uint32_t>(
    lanes_.lane_depth(MultiLaneQueue<T>::kSystemLaneSentinel));
s.num_user_lanes = lanes_.num_user_lanes();
for (uint8_t i = 0; i < s.num_user_lanes && i < 8; ++i) {
    s.lane_depths[i] = static_cast<uint32_t>(lanes_.lane_depth(i));
}
s.high_priority_depth = s.lane_depths[0];
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build -j1 && ctest --output-on-failure --parallel 8 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cli/cli_types.hpp \
        include/hpactor/mailbox/mpsc_actor_mailbox.hpp
git commit -m "feat(mailbox): expose per-lane depths in MboxSnapshot

system_lane_depth, lane_depths[8], and num_user_lanes populated from
MultiLaneQueue for CLI inspect and metrics. high_priority_depth now
reflects lane 0 depth.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 7: TOML config wiring for priority_aware / priority_levels

**Files:** Explore `src/config/parsers/` for the actor/mailbox config parser

- [ ] **Step 1: Find the parser file**

```bash
grep -rl "priority_aware\|priority_levels\|MailboxConfig" src/config/parsers/ | head -5
```

- [ ] **Step 2: Add parsing for the two new keys**

In the identified parser, follow the existing pattern for `MailboxConfig`
fields. Example (pseudocode — exact form depends on parser conventions):

```cpp
// priority_aware (optional bool, default false)
if (auto pa = table.get("priority_aware")) {
    cfg.priority_aware = pa->as_boolean();
}

// priority_levels (optional uint8, default 4)
if (auto pl = table.get("priority_levels")) {
    cfg.priority_levels = static_cast<uint8_t>(pl->as_integer());
}
```

These are parsed into `MailboxConfig` which already has the fields with
correct defaults.

- [ ] **Step 3: Build and verify**

```bash
ninja -C build -j1
```

Expected: Build succeeds without warnings.

- [ ] **Step 4: Commit**

```bash
git add src/config/parsers/<modified_file>.cpp
git commit -m "feat(config): wire priority_aware/priority_levels from TOML

Optional keys in actor mailbox config. Default to existing behaviour
(priority_aware=false, priority_levels=4).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 8: MultiLaneQueue standalone unit tests

**Files:**
- Create: `tests/unit/mailbox/test_multi_lane_queue.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

- [ ] **Step 1: Add to CMakeLists.txt**

In `tests/unit/mailbox/CMakeLists.txt`, add `test_multi_lane_queue.cpp` to the
source list for `test_unit_mailbox`.

- [ ] **Step 2: Write tests**

```cpp
// Copyright 2026 HPActor Contributors
// ...license...
#include <hpactor/mailbox/multi_lane_queue.hpp>

#include <gtest/gtest.h>

using namespace hpactor::mailbox;

namespace {

struct TestNode {
    int value = 0;
    std::atomic<TestNode*> mpsc_next{nullptr};
};

} // namespace

class MultiLaneQueueTest : public ::testing::Test {
protected:
    MultiLaneQueue<TestNode> q{4};
};

TEST_F(MultiLaneQueueTest, EnqueueDequeueSingleUserLaneFifo) {
    MultiLaneQueue<TestNode> q1{1};
    TestNode a{10}, b{20}, c{30};
    q1.enqueue(&a, 0);
    q1.enqueue(&b, 0);
    q1.enqueue(&c, 0);
    EXPECT_EQ(q1.dequeue()->value, 10);
    EXPECT_EQ(q1.dequeue()->value, 20);
    EXPECT_EQ(q1.dequeue()->value, 30);
    EXPECT_EQ(q1.dequeue(), nullptr);
}

TEST_F(MultiLaneQueueTest, SystemLaneDrainedFirst) {
    TestNode sys{1}, usr{2};
    q.enqueue(&usr, 0);
    q.enqueue(&sys, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    EXPECT_EQ(q.dequeue()->value, 1);  // system first
    EXPECT_EQ(q.dequeue()->value, 2);
}

TEST_F(MultiLaneQueueTest, UserLanesDrainedInPriorityOrder) {
    TestNode p0{0}, p1{10}, p2{20}, p3{30};
    q.enqueue(&p3, 3);
    q.enqueue(&p1, 1);
    q.enqueue(&p2, 2);
    q.enqueue(&p0, 0);
    EXPECT_EQ(q.dequeue()->value, 0);
    EXPECT_EQ(q.dequeue()->value, 10);
    EXPECT_EQ(q.dequeue()->value, 20);
    EXPECT_EQ(q.dequeue()->value, 30);
    EXPECT_EQ(q.dequeue(), nullptr);
}

TEST_F(MultiLaneQueueTest, UserLaneBeforeSystemStillSystemFirst) {
    // Even if system lane was enqueued later, it drains first.
    TestNode usr{1}, sys{2};
    q.enqueue(&sys, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    q.enqueue(&usr, 0);
    EXPECT_EQ(q.dequeue()->value, 2);
    EXPECT_EQ(q.dequeue()->value, 1);
}

TEST_F(MultiLaneQueueTest, EmptyWhenAllLanesEmpty) {
    EXPECT_TRUE(q.empty());
    TestNode n{1};
    q.enqueue(&n, 0);
    EXPECT_FALSE(q.empty());
    q.dequeue();
    EXPECT_TRUE(q.empty());
}

TEST_F(MultiLaneQueueTest, EmptyWithSystemLanePopulated) {
    TestNode n{1};
    q.enqueue(&n, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    EXPECT_FALSE(q.empty());
    q.dequeue();
    EXPECT_TRUE(q.empty());
}

TEST_F(MultiLaneQueueTest, TryDropFromLowestUserLane) {
    TestNode p0{0}, p3{30};
    q.enqueue(&p0, 0);
    q.enqueue(&p3, 3);
    TestNode* dropped = q.try_drop_from_lowest_user_lane();
    ASSERT_NE(dropped, nullptr);
    EXPECT_EQ(dropped->value, 30);
    // Lane 0 still has its message.
    EXPECT_EQ(q.dequeue()->value, 0);
    EXPECT_EQ(q.dequeue(), nullptr);
}

TEST_F(MultiLaneQueueTest, TryDropFromLowestAllEmpty) {
    EXPECT_EQ(q.try_drop_from_lowest_user_lane(), nullptr);
}

TEST_F(MultiLaneQueueTest, TryDropDoesNotTouchSystemLane) {
    TestNode sys{99};
    q.enqueue(&sys, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    EXPECT_EQ(q.try_drop_from_lowest_user_lane(), nullptr);
    EXPECT_EQ(q.dequeue()->value, 99);  // system lane intact
}

TEST_F(MultiLaneQueueTest, TotalDepthSumsAllLanes) {
    TestNode a{1}, b{2};
    q.enqueue(&a, 0);
    q.enqueue(&b, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    EXPECT_EQ(q.total_depth(), 2);
    q.dequeue();
    EXPECT_EQ(q.total_depth(), 1);
}

TEST_F(MultiLaneQueueTest, LaneDepthPerLane) {
    TestNode a{1}, b{2}, c{3};
    q.enqueue(&a, 0);
    q.enqueue(&b, 0);
    q.enqueue(&c, 2);
    EXPECT_EQ(q.lane_depth(0), 2);
    EXPECT_EQ(q.lane_depth(1), 0);
    EXPECT_EQ(q.lane_depth(2), 1);
    EXPECT_EQ(q.lane_depth(
        MultiLaneQueue<TestNode>::kSystemLaneSentinel), 0);
}

TEST_F(MultiLaneQueueTest, SetNumUserLanes) {
    EXPECT_EQ(q.num_user_lanes(), 4);
    q.set_num_user_lanes(2);
    EXPECT_EQ(q.num_user_lanes(), 2);
    q.set_num_user_lanes(8);
    EXPECT_EQ(q.num_user_lanes(), 8);
}

TEST_F(MultiLaneQueueTest, PendingFreeSetAndRelease) {
    TestNode n{42};
    q.set_pending_free(&n);
    TestNode* p = q.release_pending_free();
    EXPECT_EQ(p, &n);
    EXPECT_EQ(q.release_pending_free(), nullptr);
}

TEST_F(MultiLaneQueueTest, InjectForTest) {
    TestNode n{7};
    q.inject_for_test(&n, 2);
    EXPECT_EQ(q.lane_depth(2), 1);
    EXPECT_EQ(q.dequeue()->value, 7);
}

TEST_F(MultiLaneQueueTest, ResetClearsAllState) {
    TestNode n{1};
    q.enqueue(&n, 0);
    q.set_pending_free(new TestNode{5});  // use heap node since reset destroys it
    q.reset();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.total_depth(), 0);
    EXPECT_EQ(q.release_pending_free(), nullptr);
}
```

- [ ] **Step 3: Build and run MultiLaneQueue tests**

```bash
ninja -C build -j1 && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*MultiLaneQueue*"
```

Expected: All 15 `MultiLaneQueue*` tests pass.

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure --parallel 8 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add tests/unit/mailbox/test_multi_lane_queue.cpp \
        tests/unit/mailbox/CMakeLists.txt
git commit -m "test(mailbox): add MultiLaneQueue standalone unit tests

15 tests: enqueue/dequeue ordering, system lane priority, user lane
priority order, eviction, depth queries, lane resizing, pending_free
lifecycle, inject_for_test, and reset.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 9: Priority lanes mailbox integration tests

**Files:**
- Create: `tests/unit/mailbox/test_priority_lanes.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

- [ ] **Step 1: Add to CMakeLists.txt**

Add `test_priority_lanes.cpp` to `tests/unit/mailbox/CMakeLists.txt`.

- [ ] **Step 2: Write integration tests**

```cpp
// Copyright 2026 HPActor Contributors
// ...license...
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <gtest/gtest.h>
#include <atomic>

using namespace hpactor;
using namespace hpactor::mailbox;

namespace {

struct MockScheduler : public sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(ActorId actor, uint8_t priority,
                      int64_t deadline) override {
        last_actor = actor;
        last_priority = priority;
        last_deadline = deadline;
        notify_count.fetch_add(1, std::memory_order_relaxed);
    }
    void notify_idle(ActorId) override {}
    void yield(ActorId actor, uint8_t priority) override {
        notify_ready(actor, priority, INT64_MAX);
    }
    sched::TimerHandle schedule_after(sched::timer_callback, int64_t) override {
        return {};
    }
    sched::TimerHandle schedule_every(sched::timer_callback, int64_t) override {
        return {};
    }
    void cancel_timer(sched::TimerHandle) override {}
    size_t worker_count() const override { return 1; }
    bool is_running() const override { return true; }
    void register_dedicated_thread(ActorId, int) override {}
    void register_dedicated_pool(ActorId, uint32_t) override {}
    void unregister_dedicated(ActorId) override {}

    std::atomic<int> notify_count{0};
    ActorId last_actor{};
    uint8_t last_priority = 255;
    int64_t last_deadline = 0;
};

TypedMessage make_user_msg(uint32_t val) {
    return TypedMessage(TypeTag::User, StreamBuffer{val});
}

TypedMessage make_sys_msg() {
    return TypedMessage(TypeTag::DownMsg, StreamBuffer{1});
}

MailboxEnvelopeMeta user_meta(uint8_t prio = 0) {
    MailboxEnvelopeMeta m;
    m.type_tag = TypeTag::User;
    m.priority = prio;
    return m;
}

MailboxEnvelopeMeta sys_meta() {
    MailboxEnvelopeMeta m;
    m.type_tag = TypeTag::DownMsg;
    m.priority = 0;
    return m;
}

} // namespace

// ── Tests with priority_aware = false (default) ──────────────

class PriorityLanesDefaultTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg.capacity.max_messages = 4;
        // priority_aware defaults to false
        mbox.set_config(cfg);
    }
    MailboxConfig cfg;
    MockScheduler scheduler;
    MPSCActorMailbox<TypedMessage> mbox{ActorId{1}, &scheduler, cfg};
};

TEST_F(PriorityLanesDefaultTest, AllUserMessagesToLane0) {
    auto r1 = mbox.try_push(make_user_msg(1), user_meta(0));
    EXPECT_TRUE(r1.accepted());
    EXPECT_EQ(r1.depth, 1);
}

TEST_F(PriorityLanesDefaultTest, DequeueFifoOrder) {
    mbox.try_push(make_user_msg(10), user_meta(0));
    mbox.try_push(make_user_msg(20), user_meta(0));
    mbox.try_push(make_user_msg(30), user_meta(0));

    TypedMessage out;
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_EQ(out.payload().size(), 10u);  // StreamBuffer size is the val
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_EQ(out.payload().size(), 20u);
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_EQ(out.payload().size(), 30u);
}

// ── Tests with priority_aware = true ─────────────────────────

class PriorityLanesTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg.capacity.max_messages = 8;
        cfg.priority_aware = true;
        cfg.priority_levels = 4;
        cfg.protected_system_messages = 2;
        mbox.set_config(cfg);
    }
    MailboxConfig cfg;
    MockScheduler scheduler;
    MPSCActorMailbox<TypedMessage> mbox{ActorId{1}, &scheduler, cfg};
};

TEST_F(PriorityLanesTest, SystemMessageGoesToSystemLane) {
    auto r = mbox.try_push(make_sys_msg(), sys_meta());
    EXPECT_TRUE(r.accepted());

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.system_lane_depth, 1u);
    EXPECT_EQ(snap.depth, 1u);
}

TEST_F(PriorityLanesTest, SystemLaneCapacityRejectsWhenFull) {
    cfg.protected_system_messages = 1;
    mbox.set_config(cfg);

    auto r1 = mbox.try_push(make_sys_msg(), sys_meta());
    EXPECT_TRUE(r1.accepted());

    auto r2 = mbox.try_push(make_sys_msg(), sys_meta());
    EXPECT_FALSE(r2.accepted());
    EXPECT_EQ(r2.code, EnqueueResultCode::Rejected);
}

TEST_F(PriorityLanesTest, SystemLaneIsolatedFromUserBacklog) {
    // Fill user capacity.
    cfg.capacity.max_messages = 2;
    mbox.set_config(cfg);
    mbox.try_push(make_user_msg(1), user_meta(0));
    mbox.try_push(make_user_msg(2), user_meta(0));

    // System message should still be accepted.
    auto r = mbox.try_push(make_sys_msg(), sys_meta());
    EXPECT_TRUE(r.accepted());

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.system_lane_depth, 1u);
}

TEST_F(PriorityLanesTest, SystemMessageDequeuedBeforeUser) {
    mbox.try_push(make_user_msg(1), user_meta(0));
    mbox.try_push(make_sys_msg(), sys_meta());

    TypedMessage out;
    EXPECT_TRUE(mbox.try_pop(out));
    // First out should be the system message.
    EXPECT_TRUE(is_system_message(out.type_id()));
}

TEST_F(PriorityLanesTest, PriorityAwareRouting) {
    mbox.try_push(make_user_msg(1), user_meta(3));  // lane 3
    mbox.try_push(make_user_msg(2), user_meta(0));  // lane 0
    mbox.try_push(make_user_msg(3), user_meta(1));  // lane 1

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.lane_depths[0], 1u);
    EXPECT_EQ(snap.lane_depths[1], 1u);
    EXPECT_EQ(snap.lane_depths[2], 0u);
    EXPECT_EQ(snap.lane_depths[3], 1u);
}

TEST_F(PriorityLanesTest, PriorityAwareDequeueOrder) {
    mbox.try_push(make_user_msg(1), user_meta(3));
    mbox.try_push(make_user_msg(2), user_meta(1));
    mbox.try_push(make_user_msg(3), user_meta(0));

    TypedMessage out;
    // P0 first
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_EQ(out.payload().size(), 3u);
    // P1 second
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_EQ(out.payload().size(), 2u);
    // P3 last
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_EQ(out.payload().size(), 1u);
}

TEST_F(PriorityLanesTest, DropLowestPriorityEviction) {
    cfg.capacity.max_messages = 2;
    cfg.overflow_policy = OverflowPolicy::DropLowestPriority;
    mbox.set_config(cfg);

    mbox.try_push(make_user_msg(1), user_meta(0));  // P0
    mbox.try_push(make_user_msg(2), user_meta(3));  // P3 (lowest)

    // Third message triggers overflow — evicts from lane 3
    auto r = mbox.try_push(make_user_msg(3), user_meta(1));  // P1
    EXPECT_TRUE(r.accepted());

    auto snap = mbox.snapshot();
    // Lane 3 should be empty (evicted), lanes 0 and 1 occupied
    EXPECT_EQ(snap.lane_depths[3], 0u);
}

TEST_F(PriorityLanesTest, SnapshotPopulatesPerLaneDepths) {
    mbox.try_push(make_sys_msg(), sys_meta());
    mbox.try_push(make_user_msg(1), user_meta(0));
    mbox.try_push(make_user_msg(2), user_meta(2));

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.system_lane_depth, 1u);
    EXPECT_EQ(snap.lane_depths[0], 1u);
    EXPECT_EQ(snap.lane_depths[1], 0u);
    EXPECT_EQ(snap.lane_depths[2], 1u);
    EXPECT_EQ(snap.num_user_lanes, 4u);
    EXPECT_EQ(snap.high_priority_depth, 1u);  // lane 0 depth
}
```

- [ ] **Step 3: Build and run priority lanes tests**

```bash
ninja -C build -j1 && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*PriorityLanes*"
```

Expected: All `PriorityLanes*` tests pass.

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure --parallel 8 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add tests/unit/mailbox/test_priority_lanes.cpp \
        tests/unit/mailbox/CMakeLists.txt
git commit -m "test(mailbox): add priority lanes integration tests

11 tests covering: system lane isolation, system lane capacity
rejection, system-before-user dequeue order, priority-aware routing,
priority-aware dequeue order, DropLowestPriority eviction, per-lane
snapshot, and default FIFO preservation.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 10: Final verification

- [ ] **Step 1: Full clean build**

```bash
ninja -C build -j1
```

Expected: Zero errors, zero warnings (matching baseline).

- [ ] **Step 2: Run full test suite**

```bash
ctest --output-on-failure --parallel 8
```

Expected: All tests pass. Test count should exceed baseline by the new tests
(MultiLaneQueue + priority lanes tests).

- [ ] **Step 3: Verify no main-checkout leakage**

```bash
git -C /home/ubuntu/projects/HPActor status --short
```

Expected: Empty (clean). No files leaked to main checkout.
