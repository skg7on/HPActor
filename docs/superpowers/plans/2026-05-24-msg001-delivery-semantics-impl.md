# MSG-001 Delivery Semantics — Implementation Plan

## Summary

Implement the `DeliveryMode` API, dequeue-time deadline enforcement, receiver
dedup cache, and TOML delivery config per the design spec at
`docs/superpowers/specs/2026-05-24-msg001-delivery-semantics-design.md`.

## Prerequisites

- [x] Design spec written and reviewed.
- [x] Worktree created at `.worktrees/msg001-delivery-semantics` on branch
  `task/msg001-delivery-semantics`.
- [x] All 803 existing tests pass on the base commit (c5fadee).

## Implementation Steps

### Step 1: Create `DeliveryMode` header

**File (new):** `include/hpactor/mailbox/delivery_mode.hpp`

- Define `enum class DeliveryMode : uint8_t` with four values:
  `BestEffort = 0`, `ObservableBestEffort = 1`, `AtLeastOnce = 2`,
  `DurableAtLeastOnce = 3`.
- Define `constexpr const char* to_string(DeliveryMode)` returning
  `"best_effort"`, `"observable_best_effort"`, `"at_least_once"`,
  `"durable_at_least_once"`.
- Add full Doxygen comments per project convention.

### Step 2: Wire `DeliveryMode` into `DeliveryOptions`

**File (modify):** `include/hpactor/mailbox/mailbox_policy.hpp`

- Add `#include <hpactor/mailbox/delivery_mode.hpp>`.
- Add field to `DeliveryOptions`:
  ```cpp
  DeliveryMode delivery_mode = DeliveryMode::BestEffort;
  ```
  Place it after `emit_backpressure` and before `message_id` to minimize padding
  (bool + bool + bool + DeliveryMode = 4 bytes, then uint64_t message_id
  aligned).
- Add `is_expired()` free function:
  ```cpp
  [[nodiscard]] constexpr bool is_expired(int64_t deadline_ns,
                                           uint64_t now_ns) noexcept {
      return deadline_ns >= 0 &&
             static_cast<uint64_t>(deadline_ns) < now_ns;
  }
  ```

### Step 3: Create `DedupCache` header

**File (new):** `include/hpactor/mailbox/dedup_cache.hpp`

- Pimpl-based class with `Config` nested struct (max_entries=64K,
  ttl_ns=300s).
- Public API: `is_duplicate(endpoint, actor_id, message_id)`, `purge_expired(now_ns)`,
  `size()`, `duplicate_hits()`, `insertions()`.
- Thread safety: internal mutex. Lock-free is overkill for this cache since
  it's checked before mailbox admission, which already has a CAS path.
- Doxygen comments on all methods.

### Step 4: Create `DedupCache` implementation

**File (new):** `src/mailbox/dedup_cache.cpp`

- Internal storage: `std::unordered_map<Key, uint64_t /*insert_ns*/>` behind a
  `std::mutex`.
- Key is a 3-tuple of `(CommunicationEndpoint, ActorId, MessageId)` — hash
  combines endpoint hash, actor_id value, and message_id.
- `is_duplicate()`: check-and-insert under lock. If key exists and not expired,
  return true (duplicate). If key exists but expired, overwrite with new
  timestamp and return false. If key absent, insert and return false.
- `purge_expired()`: iterate and remove entries where `now - insert_ns > ttl`.
- Eviction: when `size() >= max_entries`, drop the oldest ~10% of entries
  (approximate LRU via a quick scan). This is safe because at-least-once
  semantics already allow spurious duplicates.

### Step 5: Add metric event types for delivery observability

**File (modify):** `include/hpactor/metrics/metrics_event.hpp`

- Add to `MetricEventType` enum:
  ```cpp
  kDeliveryDuplicate = 21,  ///< Duplicate suppressed at receiver.
  kDeliveryExpired = 22,    ///< Message expired before handler execution.
  ```

### Step 6: Wire aggregator stubs for new metric events

**File (modify):** `src/metrics/metrics_aggregator.cpp`

- Add `case MetricEventType::kDeliveryDuplicate:` and
  `case MetricEventType::kDeliveryExpired:` to the switch statement alongside
  the existing `kDeliveryFailure` stub (line 230). Aggregate as counters with
  `reason` label from `e.code` (the `FailureReason` value).

### Step 7: Add `DedupCache` ownership to `ActorSystem`

**File (modify):** `include/hpactor/core/actor_system.hpp`

- Add private member:
  ```cpp
  std::unique_ptr<mailbox::DedupCache> dedup_cache_;
  ```
  (after `dead_letters_` on line 457).
- Add public accessor:
  ```cpp
  mailbox::DedupCache* dedup_cache() { return dedup_cache_.get(); }
  ```
- Initialize in constructor body (or lazily on first `AtLeastOnce` send).

### Step 8: Update `try_deliver_local()` for delivery mode awareness

**File (modify):** `src/actor/actor_system.cpp`

Changes to `ActorSystem::try_deliver_local()` (starting ~line 395):

1. **Dedup check** — After the `get_mailbox()` lookup succeeds and before the
   deadline check: if `options.delivery_mode >= DeliveryMode::AtLeastOnce` and
   `dedup_cache_` exists, check `dedup_cache_->is_duplicate()` with
   `(endpoint_, msg.sender_id(), options.message_id)`. If duplicate:
   - Return `EnqueueResult{Accepted, target}` (sender sees success).
   - Emit `kDeliveryDuplicate` metric event.
   - Log DEBUG.

2. **Deadline enforcement (enqueue)** — Already exists implicitly via
   `try_push()`. For `ObservableBestEffort` and above, add an explicit check
   before `try_push()` using `is_expired()` so we can emit a precise
   `FailureEnvelope` with `Expired`. Drop with `kDeliveryExpired` metric.

3. **Failure envelope** — The existing failure envelope code (lines ~421-495)
   already handles `ActorNotFound` and mailbox rejection. Ensure the envelope
   `source` field is `Mailbox` for mailbox rejections and `ActorRuntime` for
   registry lookups (already correct).

### Step 9: Add dequeue-time deadline enforcement in scheduler

**File (modify):** `src/sched/scheduler.cpp`

In the dispatch path where `mailbox->try_pop(msg)` succeeds (around line 291
and line 400), before calling `actor_ptr->receive(msg)` or
`execute_actor(item)`:

```cpp
// After try_pop succeeds:
if (mailbox::is_expired(meta.deadline_ns, clock::now_ns())) {
    // Emit kDeliveryExpired metric, drop message, loop to next message
    if (metrics_ring_buffer_) { ... }
    continue; // or return to caller to re-pop
}
```

The `try_pop` needs to also return the `MailboxEnvelopeMeta` (or the
`deadline_ns`). Currently `mailbox->try_pop(msg)` only returns the message.
Need to either:
- Add a `try_pop_with_meta(msg, meta)` overload, or
- Add `deadline_ns` to `TypedMessage` metadata, or
- Add a `deadline_ns()` accessor on the mailbox that returns the most recently
  popped message's deadline.

**Preferred approach**: Add a `deadline_ns` field to `TypedMessage` (already
has sender_address). Set it in `try_push()` from `MailboxEnvelopeMeta`. This
avoids changing the mailbox interface and is the least invasive.

**Alternative**: If `TypedMessage` changes are too broad, add a
`last_popped_deadline()` accessor on the mailbox that the scheduler can query
after `try_pop()`.

### Step 10: Set deadline_ns on TypedMessage during push

**File (modify):** `include/hpactor/actor/typed_message.hpp`

- Add `int64_t deadline_ns_ = INT64_MAX` member and
  `void set_deadline_ns(int64_t)`, `int64_t deadline_ns() const` accessors.

### Step 11: Create TOML delivery config parser

**File (new):** `src/config/parsers/delivery_config_parser.cpp`

- `DeliveryConfigParser` implementing `ITomlSystemConfigParser`.
- Parses `[system.delivery]` table:
  - `default_mode` → `DeliveryMode` (string → enum).
  - `max_retries` → uint32.
  - `retry_backoff_ms` → uint32.
  - `retry_backoff_max_ms` → uint32.
  - `dedup_window_ms` → uint64.
  - `dedup_max_entries` → uint64.
- Registers via `TomlSystemParserRegistration<DeliveryConfigParser>`.
- Uses `TomlTableView` for opaque table access (no `toml.hpp` include).

### Step 12: Add `DeliveryConfig` to topology model

**File (modify):** `include/hpactor/config/topology_model.hpp`

- Add `DeliveryConfig` struct (or inline fields) to `SystemDef`:
  ```cpp
  struct DeliveryConfig {
      DeliveryMode default_mode = DeliveryMode::BestEffort;
      uint32_t max_retries = 3;
      uint32_t retry_backoff_ms = 100;
      uint32_t retry_backoff_max_ms = 10000;
      uint64_t dedup_window_ms = 300000;
      uint64_t dedup_max_entries = 65536;
  };
  DeliveryConfig delivery;
  ```
- Add to `ActorDef`: `DeliveryMode delivery_mode{DELIVERY_MODE_UNSET}` (sentinel
  for "use system default").

### Step 13: Update CMakeLists for new source files

**File (modify):** `src/CMakeLists.txt` (or wherever hpactor_lib sources are
listed)

- Add `src/mailbox/dedup_cache.cpp` to hpactor_lib.

**File (modify):** `tests/unit/mailbox/CMakeLists.txt`

- Add `test_delivery_mode.cpp`, `test_is_expired.cpp`, `test_dedup_cache.cpp`.

**File (modify):** `tests/integration/actor/CMakeLists.txt`

- Add `test_delivery_semantics.cpp`.

### Step 14: Write unit tests — `test_delivery_mode.cpp`

**File (new):** `tests/unit/mailbox/test_delivery_mode.cpp`

Tests:
- `DeliveryModeDefaultIsBestEffort` — default-constructed `DeliveryOptions` has
  `BestEffort`.
- `DeliveryModeToStringRoundTrip` — all four values produce correct strings.
- `DeliveryModeUint8Size` — `sizeof(DeliveryMode) == 1`.
- `DeliveryOptionsDefaultCompatibility` — default `DeliveryOptions{}` has
  `delivery_mode == BestEffort`, `no_drop == false`, etc.

### Step 15: Write unit tests — `test_is_expired.cpp`

**File (new):** `tests/unit/mailbox/test_is_expired.cpp`

Tests:
- `ExpiredWhenDeadlineBeforeNow` — deadline_ns=100, now_ns=200 → true.
- `NotExpiredWhenDeadlineAfterNow` — deadline_ns=200, now_ns=100 → false.
- `NotExpiredWhenNoDeadline` — deadline_ns=-1, any now → false.
- `NotExpiredWhenMaxDeadline` — deadline_ns=INT64_MAX, any now → false.
- `ExpiredAtExactBoundary` — deadline_ns=100, now_ns=100 → false (not expired,
  boundary-inclusive for now < deadline).

### Step 16: Write unit tests — `test_dedup_cache.cpp`

**File (new):** `tests/unit/mailbox/test_dedup_cache.cpp`

Tests:
- `InsertNotDuplicate` — first `is_duplicate()` returns false.
- `DuplicateDetected` — second call with same key returns true.
- `DifferentKeysNotDuplicate` — different actor_id returns false.
- `DifferentEndpointsNotDuplicate` — different source endpoint, same
  (actor_id, msg_id) → not duplicate.
- `PurgeRemovesExpired` — insert with TTL=0, purge, re-insert → not duplicate.
- `PurgePreservesUnexpired` — insert, immediate purge → still duplicate.
- `SizeIncrementsOnInsert` — size() increases after non-duplicate.
- `DuplicateHitsCounter` — counter increments on hits.
- `InsertionsCounter` — counter increments on non-duplicates.
- `StressConcurrent` — multiple threads calling `is_duplicate()` with different
  keys, no crashes, no lost entries.

### Step 17: Write integration tests — `test_delivery_semantics.cpp`

**File (new):** `tests/integration/actor/test_delivery_semantics.cpp`

Tests (use `scheduler_threads = 0` for deterministic mailbox inspection):

- `BestEffortSendReturnsVoid` — `send()` compiles and runs (smoke test).
- `TrySendBestEffortAccepted` — `try_send()` default mode → Accepted.
- `TrySendObservableBestEffortAccepted` — `try_send()` with
  `ObservableBestEffort` → Accepted.
- `TrySendAtLeastOnceAccepted` — `try_send()` with `AtLeastOnce` → Accepted.
- `TrySendAtLeastOnceDuplicateSuppressed` — two `try_send()` calls with same
  message_id → second returns Accepted but dedup cache registers hit.
- `TrySendToDeadActorReturnsNoRoute` — `try_send()` to an actor that has
  terminated → `ActorNotFound` / `FailureReason::NoRoute`.
- `ExpiredMessageNotDelivered` — enqueue with deadline in the past →
  `FailureReason::Expired`.
- `DequeueExpiredMessageNotExecuted` — spawn actor, enqueue with short deadline,
  advance time, scheduler processes → handler never called, expiry metric
  emitted.
- `DeliveryOptionsDefaultIsBestEffort` — default `DeliveryOptions` keeps
  original behavior.
- `SendSourceCompatible` — existing code that calls `send()` compiles without
  changes.

### Step 18: Build and verify

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
ctest --output-on-failure --parallel 8
```

Expected: all 803 existing tests pass + new tests pass.

## Files Changed Summary

| File | Action | Lines (est.) |
|------|--------|-------------|
| `include/hpactor/mailbox/delivery_mode.hpp` | Create | ~50 |
| `include/hpactor/mailbox/mailbox_policy.hpp` | Modify | +7 |
| `include/hpactor/mailbox/dedup_cache.hpp` | Create | ~90 |
| `include/hpactor/actor/typed_message.hpp` | Modify | +10 |
| `include/hpactor/metrics/metrics_event.hpp` | Modify | +2 |
| `include/hpactor/core/actor_system.hpp` | Modify | +5 |
| `include/hpactor/config/topology_model.hpp` | Modify | +12 |
| `src/mailbox/dedup_cache.cpp` | Create | ~120 |
| `src/actor/actor_system.cpp` | Modify | +40 |
| `src/sched/scheduler.cpp` | Modify | +25 |
| `src/metrics/metrics_aggregator.cpp` | Modify | +2 |
| `src/config/parsers/delivery_config_parser.cpp` | Create | ~70 |
| `src/CMakeLists.txt` | Modify | +1 |
| `tests/unit/mailbox/CMakeLists.txt` | Modify | +3 |
| `tests/integration/actor/CMakeLists.txt` | Modify | +1 |
| `tests/unit/mailbox/test_delivery_mode.cpp` | Create | ~60 |
| `tests/unit/mailbox/test_is_expired.cpp` | Create | ~60 |
| `tests/unit/mailbox/test_dedup_cache.cpp` | Create | ~130 |
| `tests/integration/actor/test_delivery_semantics.cpp` | Create | ~180 |

**Total**: 8 new files, 10 modified files, ~868 lines of code+tests.

## Dependencies Between Steps

```
Step 1 (DeliveryMode enum)
  ├──► Step 2 (wire into DeliveryOptions)
  ├──► Step 9 (scheduler deadline check)
  └──► Step 11 (TOML parser)

Step 3 (DedupCache header)
  └──► Step 4 (DedupCache impl)

Step 2 + Step 4 + Step 7
  └──► Step 8 (update try_deliver_local)

Step 10 (deadline_ns on TypedMessage)
  └──► Step 9 (scheduler deadline check)

Step 5 (metric events)
  └──► Step 6 (aggregator stubs)
  └──► Step 8 (emit events)

Step 12 (topology model)
  └──► Step 11 (TOML parser)

Steps 1-13 (all source changes)
  └──► Step 13 (CMakeLists)
      └──► Step 18 (build)

Steps 14-17 (tests) can be written in parallel with source changes.
```

## Rollback Plan

All changes are additive or defaulted — no existing API signature changes.
Rollback is a clean `git revert` of the commit series. No data migration or
config format change required.

## Review Checklist

- [ ] `DeliveryMode::BestEffort` is value 0 so zero-initialized structs default
  correctly.
- [ ] `is_expired()` handles `deadline_ns == -1` (no deadline) correctly.
- [ ] `DedupCache` uses internal synchronization (not `constexpr`/lock-free
  needed — it's not in the hot path).
- [ ] Dedup check happens before mailbox `try_push()`, not after — checking
  after would waste a push/pop cycle.
- [ ] Dequeue-time deadline check in scheduler handles the case where
  `try_pop()` succeeds but the message is expired — loops to next message
  rather than returning empty-handed.
- [ ] `TypedMessage::deadline_ns()` is set from `MailboxEnvelopeMeta` in
  `try_push()`, ensuring all paths (local, remote, scheduled) carry the
  deadline.
- [ ] TOML parser uses `TomlTableView` (no `toml.hpp` in header).
- [ ] Parser self-registers via `TomlSystemParserRegistration`.
- [ ] No `dynamic_cast`, `typeid`, exceptions, or RTTI.
- [ ] All existing `send()` and `try_send()` call sites compile unchanged.
- [ ] Tests use `scheduler_threads = 0` where they inspect mailbox state after
  send.
