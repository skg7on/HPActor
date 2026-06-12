# MPSCActorMailbox Thread-Safety Bugs & Formal-Method Validation Plan

Status: **Open — fixes pending**

Created: 2026-06-11

Audience: HPActor scheduler, mailbox, and concurrency maintainers.

GitHub Issue: [#258](https://github.com/skg7on/HPActor/issues/258)

## Scope

This document catalogs seven thread-safety defects discovered through formal
interleaving analysis of `MPSCActorMailbox<T>`
(`include/hpactor/mailbox/mpsc_actor_mailbox.hpp`) and its composed components
(`MultiLaneQueue`, `ReservationManager`, `PressureStateMachine`,
`OverflowQueue`, `MPSCMailbox`).

Each finding includes a formal interleaving trace (the failing concurrent
schedule), a severity classification, a concrete fix, and a verification
strategy. The final section defines a model-checking and stress-test plan to
validate the fixes and prevent regression.

---

## Formal Method

The analysis treated the system as a set of concurrent agents operating on
shared atomic and non-atomic state:

**Agents:**
- **Producer** (k = 1..N): executes `try_push()` / `enqueue()` / `enqueue_reserved()`
- **Consumer** (1): executes `dequeue()` / `try_pop()`
- **Overflow evictor** (1..N): executes `drop_one_oldest_global()` / `drop_one_lowest_priority()` (internally acquires `consumer_lock_`)
- **Drainer** (1, same as Consumer): executes `drain_overflow()` under `consumer_lock_`

**Linearization points identified:**
| Operation | Linearization point |
|-----------|-------------------|
| Producer admission | `ReservationManager::try_reserve()` CAS |
| Producer enqueue | `MPSCMailbox::enqueue()` `head_.exchange(acq_rel)` |
| Producer wakeup claim | `mailbox_was_empty_.compare_exchange_strong(acq_rel)` |
| Consumer dequeue | `consumer_lock_.test_and_set(acquire)` |
| Consumer flag reset | `mailbox_was_empty_.store(true, release)` |
| Overflow eviction | `consumer_lock_.test_and_set(acquire)` (internal) |
| Overflow drain reserve | `ReservationManager::try_reserve(0, ...)` under consumer lock |

For each suspected defect, a **failing interleaving** (a concrete concurrent
schedule) was derived by manually exploring the state space around each
linearization point, looking for schedules where the composition of correct
individual operations produces an incorrect system-level outcome.

---

## Bug 1 [CRITICAL]: Byte accounting underflow in `drain_overflow()`

### Location

`mpsc_actor_mailbox.hpp:980–1001` (`drain_overflow()`)

### Failing Interleaving

**Precondition:** `max_bytes > 0`, `overflow_policy == SpillToOverflowQueue`.
One message in overflow queue, mailbox below byte capacity.

| Step | Agent | Action |
|------|-------|--------|
| 1 | Consumer | `dequeue()` drains last user message, releases its bytes |
| 2 | Consumer | `mailbox_was_empty_.store(true)` |
| 3 | Consumer | → `drain_overflow()` |
| 4 | Consumer | `reservation_.try_reserve(0, max_msgs, max_bytes)` → **Reserved** (reserves 1 count, **0 bytes**) |
| 5 | Consumer | `overflow_queue_.try_pop(msg)` → success |
| 6 | Consumer | `estimate_node_bytes(msg)` → e.g., 256 |
| 7 | Consumer | `enqueue_reserved(new msg(256 bytes), lane=0, suppress=true)` |
| 8 | Consumer | `unlock_consumer()`, returns original message to caller |
| 9 | Worker | Actor processes returned message, requeues (mailbox has 1 msg) |
| 10 | Consumer | `dequeue()` drained message (256 bytes) |
| 11 | Consumer | `reservation_.release(256)` → **`queued_bytes_` was never incremented by 256!** |

**Result:** `queued_bytes_` (uint64_t) wraps from 0 to `2^64 - 256`. All
subsequent `try_reserve(bytes, ..., max_bytes)` calls see
`2^64 - 256 + bytes > max_bytes` → `ByteCapacity` → **all user-message
enqueues permanently rejected**. The mailbox is bricked.

### Root Cause

`drain_overflow()` calls `try_reserve(0, ...)` — bytes=0. But when the
drained message is dequeued, `dequeue()` calls `reservation_.release(bytes)`
which subtracts the **actual** byte count. The reserve and release are not
paired.

### Fix

```cpp
// BEFORE (broken):
while (config_.overflow_policy == OverflowPolicy::SpillToOverflowQueue) {
    if (reservation_.try_reserve(0, config_.capacity.max_messages,
                                 config_.capacity.max_bytes) != Reserved)
        break;
    T overflow_msg;
    if (!overflow_queue_.try_pop(overflow_msg)) {
        reservation_.release(0);
        break;
    }
    MailboxEnvelopeMeta meta;
    meta.estimated_bytes = estimate_node_bytes(overflow_msg);
    enqueue_reserved(new (...) T(std::move(overflow_msg)), meta, 0, true);
}

// AFTER (fixed):
while (config_.overflow_policy == OverflowPolicy::SpillToOverflowQueue) {
    // Pop first, then reserve with actual byte count.
    T overflow_msg;
    if (!overflow_queue_.try_pop(overflow_msg))
        break;
    uint64_t bytes = estimate_node_bytes(overflow_msg);
    auto reserve_result = reservation_.try_reserve(
        bytes, config_.capacity.max_messages, config_.capacity.max_bytes);
    if (reserve_result != Reserved) {
        // Cannot drain — push back to overflow queue.
        // If push-back fails (queue full), the message is lost;
        // increment total_lost on the overflow queue.
        if (!overflow_queue_.try_push(std::move(overflow_msg))) {
            total_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    MailboxEnvelopeMeta meta;
    meta.estimated_bytes = bytes;
    enqueue_reserved(new (...) T(std::move(overflow_msg)), meta, 0, true);
}
```

**Trade-off:** The restructured loop pops before reserving. If reservation
fails, we attempt to push back to the overflow queue. The push-back can also
fail (queue full), in which case the message is dropped. This is a bounded
loss — at most one message per drain attempt.

### Verification

- Unit test: configure `max_bytes`, enqueue messages to overflow, drain, assert
  `queued_bytes_` never underflows and subsequent enqueues succeed.
- Stress test: concurrent producers + overflow drain loop, assert byte
  accounting never negative and no permanent blocking.

---

## Bug 2 [HIGH]: Lost wakeup — TOCTOU race between `empty()` and `lanes_.enqueue()`

### Location

`mpsc_actor_mailbox.hpp:506–539` (`enqueue_reserved()`)

### Failing Interleaving

**Precondition:** Mailbox has 1 user message. `mailbox_was_empty_ == false`.
Consumer is about to dequeue it.

| Step | Producer P1 | Consumer C1 |
|------|-------------|-------------|
| 1 | `empty()` → **false** (1 msg in mailbox) | |
| 2 | | `lock_consumer()` |
| 3 | | `lanes_.dequeue()` → gets the message |
| 4 | | `reservation_.release(bytes)` |
| 5 | | `empty()` → **true** |
| 6 | | `mailbox_was_empty_.store(true, release)` |
| 7 | | `drain_overflow()` → nothing to drain |
| 8 | | `unlock_consumer()`, returns message to caller |
| 9 | `lanes_.enqueue(msg, lane)` | |
| 10 | `was_empty == false` → **skip CAS** | |
| 11 | **State: mailbox has 1 msg, `mailbox_was_empty_ == true`** | Actor is idle |

The actor is **never woken up**. The message sits in the mailbox until another
producer happens to enqueue (triggering a wakeup) or a spurious external event
reschedules the actor. Under sustained single-producer workloads after a drain,
this is an effective hang.

### Root Cause

The edge-triggered wakeup protocol gates the `mailbox_was_empty_` CAS on a
stale `empty()` snapshot. The check at step 1 and the enqueue at step 9 are
not atomic — the consumer can drain the mailbox and reset the flag between
them, making `was_empty` stale. The CAS is never attempted because the stale
snapshot says "mailbox was not empty."

The standard Vyukov MPSC wakeup protocol solves this by using the flag itself
as the sole emptiness indicator (not the queue depth), and adding a consumer-side
double-check after resetting the flag.

### Fix

**Producer side** — remove the `empty()` gate, always attempt CAS:

```cpp
// BEFORE (broken):
bool was_empty = empty();
lanes_.enqueue(node, lane_idx);
// ...
if (was_empty && !suppress_wakeup) {
    bool expected = true;
    if (mailbox_was_empty_.compare_exchange_strong(expected, false, ...)) {
        if (continuation_callback_) continuation_callback_();
        scheduler_->notify_ready(actor_id_, ...);
    }
}

// AFTER (fixed):
lanes_.enqueue(node, lane_idx);
// ...
if (!suppress_wakeup) {
    // Claim the wakeup if the flag was true (mailbox was empty at last
    // consumer observation). Spurious wakeups are safe — the consumer
    // dequeues, finds nothing, and returns to idle.
    bool expected = true;
    if (mailbox_was_empty_.compare_exchange_strong(expected, false,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
        if (continuation_callback_) continuation_callback_();
        scheduler_->notify_ready(actor_id_, meta.priority, meta.deadline_ns);
    }
}
```

**Consumer side** — add double-check after resetting the flag:

```cpp
// In dequeue(), after the node != nullptr block and drain_overflow():
if (empty()) {
    mailbox_was_empty_.store(true, std::memory_order_release);
}
drain_overflow();
unlock_consumer();
// After unlock, double-check: a producer may have enqueued between our
// dequeue and the store above. The producer's CAS would have failed
// (we set the flag to true, so expected=true would succeed), triggering
// notify_ready. But if the producer enqueued before our store landed,
// its CAS failed — we must self-requeue.
if (!lanes_.empty()) {
    bool expected = true;
    if (mailbox_was_empty_.compare_exchange_strong(expected, false,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
        scheduler_->notify_ready(actor_id_, 0, 0);
    }
}
```

The execution engine already checks `empty()` after processing and requeues
if non-empty, which covers the common case. The explicit double-check above
handles the edge case at the boundary.

### Verification

- Relacy model-checker test: 2 producers + 1 consumer, explicit schedule
  exploration for the interleaving above.
- Stress test: N producers hammering a single-consumer mailbox with random
  delays on the consumer; assert no message remains in the mailbox longer
  than a bounded time after all producers stop.
- Unit test: deterministic lost-wakeup reproduction using
  `scheduler_threads=0`, `inject_for_test()`, and manual `dequeue()`
  interleaving.

---

## Bug 3 [HIGH]: Data race on `pending_free_` via concurrent overflow eviction

### Location

`mpsc_actor_mailbox.hpp:927, 966` → `multi_lane_queue.hpp:131–137`

### Failing Interleaving

**Precondition:** Mailbox at capacity. Two producers P1, P2 both trigger
overflow → `DropOldest` policy → each calls `drop_one_oldest_global()`.

**Unsafe interleaving (concurrent read-modify-write on non-null):**

| Step | P1 (second eviction) | P2 (first eviction) |
|------|---------------------|---------------------|
| | `pending_free_` currently holds `node_old` from prior eviction | |
| 1 | reads `pending_free_` → **node_old** | reads `pending_free_` → **node_old** |
| 2 | destroys `node_old` | destroys `node_old` → **DOUBLE FREE** |
| 3 | writes `pending_free_ = node_A` | writes `pending_free_ = node_B` (overwrites A → LEAK) |

### Root Cause

`MultiLaneQueue::set_pending_free()` documents: "NOT internally locked —
caller must serialize." Both drop functions call it **after** releasing
`consumer_lock_`, violating the contract. The `set_pending_free()` body
is a non-atomic read-modify-write on `pending_free_`:

```cpp
void set_pending_free(T* node) noexcept {
    if (pending_free_) {           // read (non-atomic)
        pending_free_->~T();       // destroy
        mem::deallocate(pending_free_); // deallocate
    }
    pending_free_ = node;          // write (non-atomic)
}
```

### Fix

Move `set_pending_free()` before `unlock_consumer()` in both drop functions:

```cpp
// BEFORE (broken — both drop_one_oldest_global and drop_one_lowest_priority):
lock_consumer();
T* node = lanes_.try_drop_oldest_user_lane(); // or try_drop_from_lowest_user_lane
if (!node) { unlock_consumer(); return false; }
// ... release reservation, update counters, update pressure ...
unlock_consumer();
lanes_.set_pending_free(node);  // ← RACE
return true;

// AFTER (fixed):
lock_consumer();
T* node = lanes_.try_drop_oldest_user_lane();
if (!node) { unlock_consumer(); return false; }
// ... release reservation, update counters, update pressure ...
lanes_.set_pending_free(node);  // ← under consumer_lock_, serialized
unlock_consumer();
return true;
```

**Rationale:** `set_pending_free()` defers destruction: it stores `node` and
destroys the *previously* stored node. The actual destructor/deallocation of
`node` happens on the *next* call to `set_pending_free()` (which will also
be under the lock). This keeps destruction work minimal under the lock (at
most one destructor per call).

### Verification

- TSAN run: concurrent producers with DropOldest/DropLowestPriority overflow
  policy, high contention.
- Relacy model-checker: 2 producer threads each calling drop functions,
  explicit exploration of `pending_free_` interleavings.

---

## Bug 4 [MEDIUM]: System lane depth guard is not atomic

### Location

`mpsc_actor_mailbox.hpp:337–353` (system message path in `try_push()`)

### Failing Interleaving

**Precondition:** `protected_system_messages = 5`. Current system lane
depth = 4.

| Step | Producer P1 | Producer P2 |
|------|-------------|-------------|
| 1 | `lane_depth(kSystemLaneSentinel)` → 4 (< 5, pass) | |
| 2 | | `lane_depth(kSystemLaneSentinel)` → 4 (< 5, pass) |
| 3 | `mem::allocate()`, construct, `enqueue_reserved()` → depth = 5 | |
| 4 | | `mem::allocate()`, construct, `enqueue_reserved()` → depth = 6 |

**Result:** System lane depth = 6, exceeding the configured limit of 5.

### Root Cause

The depth check and the subsequent enqueue are not atomic. System messages
intentionally bypass `ReservationManager` (which provides atomic admission
for user messages), so there is no CAS gate.

### Fix

Add a dedicated atomic counter for system-lane admission:

```cpp
// New member:
std::atomic<uint32_t> system_lane_reserved_{0};

// In try_push(), system message path:
if (lane == MultiLaneQueue<T>::kSystemLaneSentinel) {
    uint32_t cur = system_lane_reserved_.load(std::memory_order_acquire);
    do {
        if (cur >= config_.protected_system_messages) {
            update_pressure_state(/*hard_failure=*/true);
            total_rejected_.fetch_add(1, std::memory_order_relaxed);
            // ... build rejection result ...
            return r;
        }
    } while (!system_lane_reserved_.compare_exchange_weak(
        cur, cur + 1, std::memory_order_acq_rel, std::memory_order_acquire));
    // ... allocate and enqueue ...
}

// In dequeue(), system message path, add:
if (is_sys) {
    system_lane_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
    system_lane_reserved_.fetch_sub(1, std::memory_order_release);
}
```

### Verification

- Unit test: N concurrent producers sending system messages, assert final
  system lane depth ≤ `protected_system_messages`.
- Stress test: sustained system-message flood, assert limit is never exceeded.

---

## Bug 5 [MEDIUM]: `total_rejected_` undercount on DroppedExisting retry failure

### Location

`mpsc_actor_mailbox.hpp:403–415`

### Description

When the overflow handler returns `DroppedExisting` (old message evicted,
room made), `try_push()` retries reservation. If the retry fails, the result
code is set to `Rejected` but `total_rejected_` is **not incremented**.
The overflow handler didn't increment it either (it only increments for its
own rejection path).

### Fix

```cpp
// BEFORE:
if (result.code == EnqueueResultCode::DroppedExisting) {
    reserve_result = reservation_.try_reserve(...);
    if (reserve_result == detail::ReservationResult::Reserved) {
        // ... enqueue, return success ...
    }
    result.code = EnqueueResultCode::Rejected;
}

// AFTER:
if (result.code == EnqueueResultCode::DroppedExisting) {
    reserve_result = reservation_.try_reserve(...);
    if (reserve_result == detail::ReservationResult::Reserved) {
        // ... enqueue, return success ...
    }
    result.code = EnqueueResultCode::Rejected;
    total_rejected_.fetch_add(1, std::memory_order_relaxed);
    update_pressure_state(/*hard_failure=*/true);
}
```

### Verification

- Unit test: configure DropOldest, fill mailbox to capacity, send one more,
  assert `total_dropped_ == 1` and `total_rejected_` reflects the outcome
  correctly (including retry failure).

---

## Bug 6 [LOW]: Unsynchronized access to configuration pointers from producer threads

### Locations

| Member | Type | Read location | Write location |
|--------|------|---------------|----------------|
| `admission_policies_` | `shared_ptr<vector<...>>` | `try_push()` L317 | `set_admission_policies()` |
| `rate_limiter_` | `unique_ptr<ActorRateLimiter>` | `dequeue()` L569 | `set_rate_limiter()` |
| `continuation_callback_` | `std::function<void()>` | `enqueue_reserved()` L534 | `set_continuation_callback()` |
| `metrics_ring_buffer_` | `MpscRingBuffer*` (raw) | multiple hot paths | `set_metrics_ring_buffer()` |
| `logger_` | `Logger*` (raw) | `enqueue_reserved()` | `set_logger()` |

None of these types provide safe concurrent read/write. The documentation
states "set before concurrent use," but the public API allows runtime
reconfiguration and there is no enforcement.

### Fix

**Option A (preferred for now):** Document and enforce the quiescence contract.
Add a debug-mode assertion that fires if any setter is called while the
mailbox is in active use.

**Option B (robust):** Use `std::atomic<std::shared_ptr<T>>` (C++20) for
`admission_policies_`, wrap `rate_limiter_` in an atomic, and make
`continuation_callback_` an atomic wrapper. This allows lock-free runtime
reconfiguration.

**Option C (pragmatic):** Document that all `set_*` methods must be called
from the actor's own thread during a period when no messages are in flight.
This is already the de-facto contract for `set_config()`.

### Verification

- TSAN run with a test that reconfigures the mailbox at runtime while
  producers are active.

---

## Bug 7 [LOW]: `inject_for_test()` bypasses lane routing for system messages

### Location

`mpsc_actor_mailbox.hpp:729–734`

### Description

`inject_for_test()` always enqueues to lane 0, regardless of whether the
message is a system message. If the node is a system message, it should
go to the system lane and update `system_lane_bytes_`, not the user-message
`reservation_`.

### Fix

```cpp
// BEFORE:
void inject_for_test(T* node) noexcept {
    reservation_.inject_count(estimate_node_bytes(*node));
    total_enqueued_.fetch_add(1, std::memory_order_relaxed);
    lanes_.enqueue(node, 0);
    mailbox_was_empty_.store(false, std::memory_order_release);
}

// AFTER:
void inject_for_test(T* node) noexcept {
    uint64_t bytes = estimate_node_bytes(*node);
    uint8_t lane = 0;
    if constexpr (std::is_same_v<T, TypedMessage>) {
        if (is_system_message(node->type_id())) {
            lane = MultiLaneQueue<T>::kSystemLaneSentinel;
            system_lane_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    if (lane != MultiLaneQueue<T>::kSystemLaneSentinel) {
        reservation_.inject_count(bytes);
    }
    total_enqueued_.fetch_add(1, std::memory_order_relaxed);
    lanes_.enqueue(node, lane);
    mailbox_was_empty_.store(false, std::memory_order_release);
}
```

---

## Formal-Method Validation Plan

### Phase 1: Relacy Model Checker

Port the core MPSC enqueue/dequeue/wakeup protocol to
[Relacy](https://github.com/dvyukov/relacy) (or equivalent C++ model checker)
for exhaustive state-space exploration.

**Scope:**
- `MPSCMailbox::enqueue()` + `MPSCMailbox::dequeue()` with 2 producers + 1 consumer
- `mailbox_was_empty_` wakeup protocol (Bug 2 fix validation)
- `MultiLaneQueue::set_pending_free()` concurrent access (Bug 3 fix validation)
- `ReservationManager::try_reserve()` + `release()` paired accounting (Bug 1 fix validation)

**Success criteria:** 0 data races, 0 assertion failures, 0 lost wakeups
across full state-space exploration with up to 3 threads and bounded
message counts (up to 5 per producer).

### Phase 2: Stress Tests (TSAN + ASAN)

**Test 1: Concurrent producer flood + bounded capacity**
- 8 producer threads, 1 consumer, mailbox capacity = 64
- Each producer sends 10,000 messages
- Overflow policy: DropOldest
- Assert: `total_enqueued + total_rejected + total_dropped + total_dead_letters == total_sent`
- Assert: `queued_bytes_` never exceeds `max_bytes` (and never underflows)
- Assert: system lane depth never exceeds `protected_system_messages`
- Assert: `mailbox_was_empty_` is consistent with actual emptiness after quiescence

**Test 2: Overflow drain stress**
- Fill overflow queue to capacity, then drain via consumer
- Assert: byte accounting remains consistent (no underflow)
- Assert: all overflow messages eventually dequeued

**Test 3: Lost wakeup stress**
- 1 producer, 1 slow consumer (random delays)
- Producer sends 1,000 messages with 1ms gaps
- Consumer has random 0–5ms processing delays
- After all sends complete, assert consumer drains all messages within 5s timeout
- Repeat with 16 producers

**Test 4: Pending-free race stress**
- 8 producers, DropOldest policy, mailbox capacity = 4
- Each producer sends 5,000 messages
- Run under TSAN, assert 0 races on `pending_free_`

### Phase 3: Deterministic Unit Tests (scheduler_threads = 0)

**Test 1: Drain byte accounting**
- Setup: mailbox with `max_bytes = 1024`, overflow queue with 1 message
- Manually call `drain_overflow()` and verify `queued_bytes_` matches expected value
- Dequeue the drained message, verify `queued_bytes_` returns to 0

**Test 2: Wakeup protocol interleaving**
- Inject 1 message via `inject_for_test()`, set `mailbox_was_empty_ = false`
- Call `dequeue()` to drain it (should set `mailbox_was_empty_ = true`)
- Simultaneously call `try_push()` — verify wakeup is triggered

**Test 3: System lane depth guard**
- Set `protected_system_messages = 2`
- Send 4 system messages from 4 "producers" (single-threaded simulation)
- Assert no more than 2 are enqueued

**Test 4: DroppedExisting retry accounting**
- Fill mailbox to capacity, trigger DropOldest overflow
- Ensure retry fails (by making another thread grab the freed slot)
- Assert `total_rejected_` is incremented

---

## Verification Checklist

- [ ] Bug 1 (drain byte underflow): fix implemented, Relacy + unit test pass
- [ ] Bug 2 (lost wakeup): fix implemented, Relacy + stress test pass
- [ ] Bug 3 (pending_free race): fix implemented, TSAN clean
- [ ] Bug 4 (system lane guard): fix implemented, unit test pass
- [ ] Bug 5 (rejected undercount): fix implemented, unit test pass
- [ ] Bug 6 (unsynchronized config): quiescence assertion or atomic wrappers added
- [ ] Bug 7 (inject_for_test routing): fix implemented
- [ ] Phase 1 Relacy: all models pass with 0 violations
- [ ] Phase 2 TSAN: `ctest -R mpsc_stress` clean under TSAN
- [ ] Phase 2 ASAN: `ctest -R mpsc_stress` clean under ASAN
- [ ] Phase 3 deterministic: all new unit tests pass
- [ ] Full test suite: `ctest --output-on-failure --parallel 8` passes
- [ ] `AGENTS.md` / `CLAUDE_MEMORY.md` updated with new concurrency invariants

---

## References

- `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md` — normative rules
- `include/hpactor/adt/mpsc_mailbox.hpp` — Vyukov MPSC queue
- `include/hpactor/adt/reservation_manager.hpp` — two-phase atomic reservation
- `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` — primary file under analysis
- `include/hpactor/mailbox/multi_lane_queue.hpp` — lane container
- `include/hpactor/mailbox/overflow_queue.hpp` — mutex-protected spill queue
- `include/hpactor/mailbox/detail/pressure_state_machine.hpp` — hysteresis state machine

---

# Appendix: Implementation Plan

Status: **Approved — ready for writing-plans**

## Implementation Order

Bugs ordered by dependency chain — simpler first to build momentum, complex
protocol changes last:

| Step | Bug | Complexity | Files Touched |
|------|-----|-----------|---------------|
| 1 | Bug 3 — `pending_free_` race | Low | `mpsc_actor_mailbox.hpp` |
| 2 | Bug 5 — `total_rejected_` undercount | Low | `mpsc_actor_mailbox.hpp` |
| 3 | Bug 7 — `inject_for_test` routing | Low | `mpsc_actor_mailbox.hpp` |
| 4 | Bug 6 — unsynchronized config | Low | `mpsc_actor_mailbox.hpp` |
| 5 | Bug 4 — system lane guard | Medium | `mpsc_actor_mailbox.hpp` |
| 6 | Bug 1 — drain byte underflow | High | `mpsc_actor_mailbox.hpp` |
| 7 | Bug 2 — lost wakeup | High | `mpsc_actor_mailbox.hpp` |

For each bug: RED (failing test) → GREEN (minimal fix) → REFACTOR (cleanup).

## Production Code Changes

**Primary file:** `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

**Bug 3 fix** — Move `set_pending_free(node)` before `unlock_consumer()` in
`drop_one_oldest_global()` and `drop_one_lowest_priority()`.

**Bug 5 fix** — Add `total_rejected_.fetch_add(1, relaxed)` and
`update_pressure_state(true)` in the DroppedExisting retry-failure path.

**Bug 7 fix** — Add system-message detection to `inject_for_test()`: route to
system lane sentinel when `is_system_message(node->type_id())`, update
`system_lane_bytes_` accordingly.

**Bug 6 fix** — Add `#ifdef HPACTOR_DEBUG` atomic `in_active_use_` flag.
Assert it's false in `set_rate_limiter()`, `set_admission_policies()`,
`set_continuation_callback()`, `set_metrics_ring_buffer()`, `set_logger()`.
Set to true on first `try_push()` or `dequeue()`.

**Bug 4 fix** — New member `std::atomic<uint32_t> system_lane_reserved_{0}`.
Producer: CAS-based admission loop replacing `lane_depth() >= limit` check.
Consumer: `fetch_sub(1, release)` in dequeue system message path.

**Bug 1 fix** — Restructure `drain_overflow()`: pop first, estimate bytes,
reserve with actual byte count. On reservation failure, push back to overflow
queue (drop if push-back fails).

**Bug 2 fix** — Two-sided protocol change:
- Producer: remove `empty()` gate, always attempt CAS on `mailbox_was_empty_`
- Consumer (`dequeue()`): add double-check after `unlock_consumer()` —
  if `!lanes_.empty()`, CAS the flag and self-requeue via `notify_ready()`

## New Test Files

### `tests/unit/mailbox/test_mailbox_formal_validation.cpp`

Deterministic tests, `scheduler_threads=0`, mock `IActorReadyNotifier`:

| Suite | Tests | Bug |
|-------|-------|-----|
| DrainOverflowByteAccounting | UnderflowWhenDrainReservesZeroBytes, SubsequentEnqueuesSucceedAfterDrain, PushBackOnReservationFailure | 1 |
| WakeupProtocolRace | ProducerWakeupAfterConsumerResetsFlag, NoWakeupWhenMailboxNotEmpty, ConsumerDoubleCheckCatchesConcurrentEnqueue | 2 |
| SystemLaneDepthGuard | RejectsWhenAtLimit, ReleasesOnDequeue | 4 |
| DroppedExistingRetryAccounting | RejectsWithIncrementedCounter_RetryFailure, RejectsWithIncrementedCounter_RetrySuccess | 5 |

### `tests/unit/mailbox/test_mailbox_stress_formal.cpp`

Multi-threaded, designed for TSAN/ASAN:

| Suite | Tests | Bug |
|-------|-------|-----|
| ConcurrentProducerFlood | ByteAccountingNeverUnderflows, AllMessagesAccountedFor, SystemLaneDepthNeverExceedsLimit | 1,2,4 |
| OverflowDrainConsistency | DrainPreservesMessageCount, ByteAccountingStaysBounded | 1 |
| LostWakeupDetection | NoMessagesLeftBehind_SingleProducer, NoMessagesLeftBehind_MultiProducer | 2 |
| PendingFreeConcurrency | NoDoubleFreeUnderTSAN, NoLeakedNodesAfterQuiescence | 3 |

### `tests/unit/mailbox/test_mpsc_relacy.cpp` (extend)

New Relacy suites following existing `MPSC_2Producers` pattern:

| Suite | Threads | Description |
|-------|---------|-------------|
| MPSC_WakeupProtocol | 3 | 2 producers + 1 consumer, exhaustive schedule exploration of wakeup flag protocol |
| MPSC_PendingFreeRace | 3 | 2 evictors calling set_pending_free, explore all interleavings |
| MPSC_ReservationPairing | 3 | Interleave try_reserve/release with drain_overflow-style reserve(0), verify byte accounting never wraps |

## Build Changes

`tests/unit/mailbox/CMakeLists.txt` — add two new test targets:

```cmake
add_mailbox_test(test_mailbox_formal_validation)
add_mailbox_test(test_mailbox_stress_formal)
```

## Verification Gating

Each bug is complete when:
1. RED: new test reproduces the failure
2. GREEN: fix applied, test passes
3. REFACTOR: code clean, test still passes

Per-bug verification commands (narrowest scope):

| Step | Verify |
|------|--------|
| Bug 3 | `./build/tests/unit/mailbox/test_mailbox_stress_formal --gtest_filter="*PendingFree*"` (TSAN) |
| Bug 5 | `./build/tests/unit/mailbox/test_mailbox_formal_validation --gtest_filter="*DroppedExisting*"` |
| Bug 7 | `./build/tests/unit/mailbox/test_mailbox_formal_validation --gtest_filter="*InjectForTest*"` |
| Bug 6 | Build with `-DHPACTOR_DEBUG=ON`, run existing tests |
| Bug 4 | `./build/tests/unit/mailbox/test_mailbox_formal_validation --gtest_filter="*SystemLane*"` |
| Bug 1 | `./build/tests/unit/mailbox/test_mailbox_formal_validation --gtest_filter="*DrainOverflow*"` |
| Bug 2 | `./build/tests/unit/mailbox/test_mpsc_relacy --gtest_filter="*Wakeup*"` |

Full verification after all fixes:
```bash
ctest --output-on-failure --parallel 8
ctest -R "mpsc_stress|mailbox_stress" --output-on-failure  # under TSAN build
