# Priority-Aware Mailbox Lanes & Protected System-Message Lane

## 1. Executive Summary

MBX-005 adds priority-aware multi-lane mailboxes and a dedicated protected
system-message lane. Today the mailbox is a single FIFO MPSC queue embedded
directly in `MPSCActorMailbox`. Adding lanes on top of the current 530-line
class would make it unmaintainable.

This design therefore proceeds in two parts:

1. **Refactor** — extract queue storage, dequeue ordering, and lane iteration
   into a standalone `MultiLaneQueue<T>`. The `MPSCActorMailbox` shrinks to
   an orchestrator: admission control, pressure, overflow, metrics.
2. **Feature** — implement system-lane isolation, priority-aware routing,
   `DropLowestPriority`, and per-lane observability on the clean abstraction.

The default (`priority_aware = false`) preserves existing single-lane FIFO
behavior with zero overhead beyond the abstraction boundary.

## 2. Current State

### 2.1 What already exists

| Component | File | State |
|-----------|------|-------|
| `MailboxConfig::priority_levels` | `mailbox_policy.hpp:60` | Declared (`uint8_t`, default 4), unused |
| `MailboxConfig::priority_aware` | `mailbox_policy.hpp:69` | Declared (`bool`, default false), unused |
| `MailboxConfig::protected_system_messages` | `mailbox_policy.hpp:66` | Declared (`uint32_t`, default 32), used |
| `MailboxEnvelopeMeta::priority` | `mailbox_policy.hpp:89` | Declared (`uint8_t`, default 0) |
| `ReservationManager::try_reserve_system()` | `reservation_manager.hpp:66` | Implemented |
| `sched::IScheduler::notify_ready(actor, priority, deadline_ns)` | `scheduler.hpp:99` | Declared, scheduler has priority queues |
| `OverflowPolicy::DropLowestPriority` | `mailbox_policy.hpp:37` | Enum value, falls through to `RejectNewest` |
| `MboxSnapshot::high_priority_depth` | `cli_types.hpp:50` | Field exists, hardcoded to 0 |
| `is_system_message()` | `mailbox_policy.hpp:204` | TypeTag-based detection |

### 2.2 Current `MPSCActorMailbox` concerns (530 lines, one class)

```
+----------------------------------------------------------+
|                  MPSCActorMailbox<T>                      |
|                                                           |
|  +----------+  +----------------+  +------------------+  |
|  | Queue    |  | Admission      |  | Overflow         |  |
|  | (1 lane) |  | (reservation   |  | (policy,         |  |
|  |          |  |  mgr, press-   |  |  handler,        |  |
|  |          |  |  ure state,    |  |  drop_oldest,    |  |
|  |          |  |  backpressure  |  |  drain_ovfl)     |  |
|  |          |  |  gate)         |  |                  |  |
|  +----------+  +----------------+  +------------------+  |
|  +----------+  +----------------+  +------------------+  |
|  | Consumer |  | Metrics        |  | Config           |  |
|  | (lock,   |  | (ring buf,     |  | (set_config,     |  |
|  |  pending |  |  counters,     |  |  const access)   |  |
|  |  free)   |  |  snapshot)     |  |                  |  |
|  +----------+  +----------------+  +------------------+  |
+----------------------------------------------------------+
```

All concerns are mixed in one template. The queue storage (`MPSCMailbox<T>
mailbox_`) is accessed directly by enqueue, dequeue, `drop_one_oldest`,
`drain_overflow`, `empty`, `snapshot`, and `inject_for_test`. Adding lanes
would multiply every access point by N+1, producing spaghetti.

### 2.3 What is missing

- No separate system-message queue — system messages stall behind user backlog.
- No multi-lane dequeue — single `MPSCMailbox<T>` is always FIFO.
- `DropLowestPriority` has no implementation.
- Per-lane depth metrics don't exist.
- Priority routing is absent — `MailboxEnvelopeMeta::priority` is never used
  for lane selection.

## 3. Goals

1. Refactor queue storage out of `MPSCActorMailbox` into a testable
   `MultiLaneQueue<T>` component.
2. Add a dedicated system-message lane with its own queue and capacity.
3. Add N user-priority lanes (configurable, default 4) behind a
   `priority_aware` toggle.
4. Drain system lane first, then user lanes in priority order (0 = highest).
5. Implement `DropLowestPriority` overflow policy.
6. Preserve source compatibility: default config behaves exactly as today.
7. Expose per-lane depths through metrics and CLI.
8. Keep producers lock-free (MPSC per lane), consumer single-threaded.

## 4. Non-Goals

- Per-lane capacity limits (all user lanes share total capacity).
- Weighted round-robin or starvation prevention between user lanes.
- Priority inheritance or dynamic adjustment.
- Remote transport priority lanes (MBX-006).
- Changing scheduler priority/EDF queues.
- Per-lane backpressure signaling (aggregate only in v1).

## 5. Design

### 5.1 Refactoring: extract `MultiLaneQueue<T>`

The core extraction is a new header-only component that owns the queue
storage and provides a narrow consumer/producer interface. The mailbox
orchestrates around it without touching MPSC internals.

```
+----------------------------------------------------------+
|                  MultiLaneQueue<T>                        |
|                                                           |
|  Owns:                                                    |
|    system_lane_  : MPSCMailbox<T>  (always present)       |
|    user_lanes_[] : MPSCMailbox<T>  (N = priority_levels)  |
|    pending_free_ : T*                                    |
|                                                           |
|  Provides:                                                |
|    enqueue(T* node, LaneIdx)    — producer (lock-free)    |
|    dequeue() -> T*              — consumer (priority)     |
|    try_drop_from_lowest() -> bool — consumer eviction     |
|    empty() -> bool                                        |
|    total_depth() -> int64_t                               |
|    lane_depth(LaneIdx) -> int64_t                         |
|    num_user_lanes() -> uint8_t                            |
|    set_num_user_lanes(uint8_t n) — resize user lanes      |
|    reset()                         — for tests            |
|                                                           |
|  Does NOT own or know about:                              |
|    - reservation / admission                              |
|    - pressure state                                       |
|    - overflow policies                                    |
|    - metrics ring buffer                                  |
|    - scheduler wakeup                                     |
|    - config                                               |
+----------------------------------------------------------+
```

**Lane index space:**

```
  System lane:  LaneIdx = 0xFF (kSystemLaneSentinel)
  User lane 0:  LaneIdx = 0    (highest priority)
  User lane 1:  LaneIdx = 1
  ...
  User lane N-1: LaneIdx = N-1 (lowest priority)

  Dequeue order: system -> lane 0 -> lane 1 -> ... -> lane N-1
```

**Key design decision — `pending_free_` moves into `MultiLaneQueue`:**

The `pending_free_` pointer (deferred destruction of a dequeued node) is
tightly coupled to the dequeue path and the queue element type. It has
nothing to do with admission, pressure, or overflow policy. Moving it
into `MultiLaneQueue` keeps the destructor concern with the storage that
produced the node.

**Why `MultiLaneQueue` does not own the consumer lock:**

The consumer lock serializes dequeue AND overflow eviction AND
`drain_overflow`. These three paths span both the queue storage and
the reservation manager. The lock belongs to the orchestrator
(`MPSCActorMailbox`) because it protects the cross-cutting invariant
"at most one thread is draining or evicting."

**`MultiLaneQueue` dequeue contract:**

`dequeue()` is NOT internally locked. The caller (`MPSCActorMailbox`)
holds `consumer_lock_` before calling it. This keeps the spinlock
in the orchestrator where it can also protect reservation updates
during eviction.

**`MultiLaneQueue` API:**

```cpp
template <typename T>
class MultiLaneQueue {
public:
    static constexpr uint8_t kSystemLaneSentinel = 0xFF;
    static constexpr uint8_t kMaxUserLanes = 8;

    explicit MultiLaneQueue(uint8_t num_user_lanes = 1);

    // Producer (lock-free, multi-producer safe)
    void enqueue(T* node, uint8_t lane_idx) noexcept;

    // Consumer (NOT internally locked — caller serializes)
    T* dequeue() noexcept;

    // Eviction: drop one message from the lowest-priority non-empty
    // user lane. Returns false if all user lanes are empty.
    // NOT internally locked — caller holds consumer lock.
    bool try_drop_from_lowest_user_lane() noexcept;

    // Deferred destructor for the last dequeued node.
    void set_pending_free(T* node) noexcept;
    T* release_pending_free() noexcept;

    bool empty() const noexcept;
    int64_t total_depth() const noexcept;
    int64_t lane_depth(uint8_t lane_idx) const noexcept;
    uint8_t num_user_lanes() const noexcept;

    // Resize user lane array. Dropped messages in removed lanes
    // are leaked — caller must drain lanes before shrinking.
    void set_num_user_lanes(uint8_t n);

    // Test support: inject directly without reservation.
    void inject_for_test(T* node, uint8_t lane_idx) noexcept;

    // Reset all state (for tests).
    void reset() noexcept;

private:
    MPSCMailbox<T> system_lane_;
    std::vector<MPSCMailbox<T>> user_lanes_;
    T* pending_free_{nullptr};
};
```

### 5.2 Refactored `MPSCActorMailbox` after extraction

After `MultiLaneQueue<T>` is extracted, the mailbox becomes an orchestrator:

```
+----------------------------------------------------------+
|           MPSCActorMailbox<T>  (orchestrator)              |
|                                                           |
|  Composes:                                                |
|    lanes_          : MultiLaneQueue<T>                    |
|    reservation_    : ReservationManager                   |
|    pressure_state_ : PressureStateMachine                 |
|    backpressure_   : BackpressureSignalGate               |
|    overflow_handler_: IOverflowHandler<T>                 |
|    overflow_queue_ : OverflowQueue<T>                     |
|                                                           |
|  Owns:                                                    |
|    consumer_lock_  : atomic_flag                          |
|    config_         : MailboxConfig                        |
|    counters        : atomic<uint64_t> (enq/deq/rej/...)  |
|    metrics_ring_buffer_                                   |
|    logger_                                                |
|    scheduler_                                             |
|                                                           |
|  Public API unchanged.                                    |
+----------------------------------------------------------+
```

The orchestrator's `try_push` flow:

```
try_push(msg, meta):
  lane = route(meta)   // system -> kSystemLaneSentinel, user -> priority->lane

  if lane is system:
    if lanes_.lane_depth(system) >= config_.protected_system_messages:
      return Rejected   // system lane full, never drop
    // fall through to allocate+enqueue (no reservation needed)

  else:  // user lane
    result = reservation_.try_reserve(bytes, max_msgs, max_bytes)
    if result != Reserved:
      // overflow handling (uses lanes_ for DropLowestPriority eviction)
      ...

  node = allocate+construct(msg)
  lanes_.enqueue(node, lane)
  update_pressure_state()
  if was empty: scheduler_->notify_ready(actor_id_, meta.priority, meta.deadline_ns)
  emit_metrics()
  return Accepted
```

The orchestrator's `dequeue` flow:

```
dequeue():
  lock_consumer()
  node = lanes_.dequeue()          // system first, then user lanes P0..PN
  if node:
    update_reservation_release(node)
    lanes_.set_pending_free(node)
    update_pressure_state()
    drain_overflow()               // unchanged logic, enqueues to lane 0
  unlock_consumer()
  emit_metrics()
  return node
```

### 5.3 Message routing

```cpp
uint8_t route(const MailboxEnvelopeMeta& meta) const noexcept {
    if (is_system_message(meta.type_tag))
        return MultiLaneQueue<T>::kSystemLaneSentinel;
    if (!config_.priority_aware)
        return 0;
    return std::min<uint8_t>(meta.priority, lanes_.num_user_lanes() - 1);
}
```

**System messages**: Always system lane, regardless of `priority_aware`.
**User, `priority_aware = false`**: All to lane 0 (current behaviour).
**User, `priority_aware = true`**: Lane = `min(priority, N-1)`.

### 5.4 System lane capacity

The system lane has dedicated capacity: `config_.protected_system_messages`.
Admission checks `lanes_.lane_depth(kSystemLaneSentinel) < limit`. When full,
system messages are rejected (no drop/overflow — losing system messages is
worse than losing user messages).

The existing `ReservationManager::try_reserve_system()` and
`reserved_system_count()` are **removed**. The system lane's own
`MPSCMailbox::count()` is the authoritative occupancy counter. This
eliminates a source of divergence between the counter and the actual
queue depth.

### 5.5 ReservationManager changes

`ReservationManager` shrinks to user-lane-only tracking:

```cpp
template <typename T>
class ReservationManager {
public:
    ReservationResult try_reserve(uint64_t bytes, uint32_t max_messages,
                                  uint64_t max_bytes) noexcept;
    void release(uint64_t bytes) noexcept;
    void inject_count(uint64_t bytes) noexcept;
    uint32_t reserved_count() const noexcept;
    uint64_t queued_bytes() const noexcept;
private:
    std::atomic<uint32_t> reserved_messages_{0};
    std::atomic<uint64_t> queued_bytes_{0};
};
```

Removed: `try_reserve_system()`, `release_system()`, `reserved_system_count()`.
Callers that previously used them now check `lanes_.lane_depth(kSystemLaneSentinel)`
directly.

### 5.6 `DropLowestPriority` overflow policy

New handler `DropLowestPriorityHandler<T>`:

```cpp
template <typename T>
class DropLowestPriorityHandler : public IOverflowHandler<T> {
public:
    explicit DropLowestPriorityHandler(MultiLaneQueue<T>* lanes)
        : lanes_(lanes) {}

    EnqueueResult handle(OverflowContext<T>& ctx,
                         ReservationResult reason) noexcept override {
        if (lanes_->try_drop_from_lowest_user_lane()) {
            ctx.total_dropped++;
            EnqueueResult r;
            r.code = EnqueueResultCode::DroppedExisting;
            return r;  // caller retries enqueue
        }
        return ctx.make_rejected();
    }
private:
    MultiLaneQueue<T>* lanes_;
};
```

`try_drop_from_lowest_user_lane()` iterates user lanes in reverse order
(N-1 down to 0), dequeues the first non-empty one, and returns true.
The caller (`try_push`) is responsible for releasing the reservation
via `reservation_.release(bytes)`.

`OverflowContext` gains:
```cpp
MultiLaneQueue<T>* lanes = nullptr;
```

### 5.7 `drop_one_oldest` changes

This private method is used by `DropOldest` and `DropNewest` overflow
handlers. It now delegates to `lanes_.try_drop_from_lowest_user_lane()`,
which naturally picks the oldest message in the lowest-priority lane
(within each lane, MPSC preserves FIFO).

The existing `drop_one_oldest()` method on `MPSCActorMailbox` is replaced
by a call to `lanes_.try_drop_from_lowest_user_lane()`.

### 5.8 `drain_overflow` changes

Unchanged logic: dequeue from `overflow_queue_`, enqueue to user lane 0.
Overflow-drained messages go to the default lane regardless of original
priority. v1 keeps this simple; overflow spill is rare.

### 5.9 Pressure state

Computed on aggregate: `total_depth() = lanes_.total_depth()` across all
lanes (system + user). The existing `pressure_ratio()` and
`update_pressure_state()` logic is unchanged except that depth comes
from `lanes_.total_depth()` instead of `mailbox_.count()`.

### 5.10 Configuration

Per-actor TOML:
```toml
[actor.my_service]
priority_aware = true
priority_levels = 4              # 1-8, default 4
protected_system_messages = 32   # system lane capacity
```

System default in `[hpactor.mailbox]`:
```toml
[hpactor.mailbox]
priority_aware = false
priority_levels = 4
protected_system_messages = 32
```

When `priority_aware = false` and `priority_levels = 1`, the system
creates one user lane — identical to today.

### 5.11 Metrics and CLI

`MboxSnapshot` gains:
```cpp
uint32_t system_lane_depth = 0;
uint32_t lane_depths[MultiLaneQueue<T>::kMaxUserLanes] = {};
uint8_t num_user_lanes = 1;
```

The existing `high_priority_depth` field is populated from `lane_depths[0]`.

New metrics ring buffer events:
- `kMailboxSystemLaneFull` — system lane rejected a message
- `kMailboxEnqueueLane` — enqueue with `value_lo = lane_idx`

CLI `inspect` shows per-lane depths when `priority_aware` is enabled:
```
Mailbox:
  depth:      42 / 1024
  system:     2 / 32
  lanes:      [12, 18, 8, 4]  (P0..P3)
  pressure:   normal (0.04)
```

### 5.12 File layout

```
include/hpactor/mailbox/
  mpsc_mailbox.hpp              (unchanged — single MPSC queue)
  multi_lane_queue.hpp          (NEW — MultiLaneQueue<T>)
  mpsc_actor_mailbox.hpp        (refactored — shrinks, orchestrates)
  detail/
    reservation_manager.hpp     (simplified — system methods removed)
    overflow_context.hpp        (gains MultiLaneQueue* lane pointer)
    handlers/
      drop_lowest_priority_handler.hpp  (NEW)
    overflow_handler_factory.hpp        (wired to new handler)

tests/unit/mailbox/
  test_multi_lane_queue.cpp     (NEW — standalone lane queue tests)
  test_priority_lanes.cpp       (NEW — integration with mailbox)
```

## 6. Implementation Plan

### Phase 0: Extract `MultiLaneQueue<T>` (refactor, no feature change)

1. Create `include/hpactor/mailbox/multi_lane_queue.hpp` with
   `MultiLaneQueue<T>` containing one system lane + one user lane.
2. Move queue storage out of `MPSCActorMailbox`: replace `MPSCMailbox<T>
   mailbox_` with `MultiLaneQueue<T> lanes_(1)`.
3. Move `pending_free_` into `MultiLaneQueue`.
4. Replace all direct `mailbox_.xxx()` calls with `lanes_.xxx()`:
   - `mailbox_.enqueue(node)` -> `lanes_.enqueue(node, lane_idx)`
   - `mailbox_.dequeue()` -> `lanes_.dequeue()`
   - `mailbox_.empty()` -> `lanes_.empty()`
   - `mailbox_.count()` -> `lanes_.total_depth()`
5. `dequeue()` now calls `lanes_.dequeue()` (internally drains system
   lane first, then user lane 0 — system lane is always empty in this
   phase since system messages still go through user lane 0 via the
   existing `try_reserve_system` bypass).
6. Run full test suite. All existing tests must pass unchanged.
7. Commit: "refactor(mailbox): extract MultiLaneQueue from
   MPSCActorMailbox"

Files touched: `multi_lane_queue.hpp` (new), `mpsc_actor_mailbox.hpp`.

**Why Phase 0 must pass all existing tests:** This is a pure mechanical
refactor. The system lane exists but is never populated (system messages
still route to user lane 0 via the existing code path). No observable
behavior changes.

### Phase 1: Route system messages to the system lane

8. Add `route(meta)` helper to `MPSCActorMailbox`.
9. In `try_push`: when `is_system_message(meta.type_tag)`, route to
   system lane, check `lane_depth(system) < protected_system_messages`,
   enqueue directly (no user-lane reservation).
10. In `dequeue`: system messages now come from system lane via
    `lanes_.dequeue()` (which already drains system lane first).
11. Remove `ReservationManager::try_reserve_system()` and
    `reserved_system_count()`.
12. Update tests: system messages land in system lane, isolated from
    user capacity.

Files touched: `mpsc_actor_mailbox.hpp`, `reservation_manager.hpp`.

### Phase 2: Priority-aware routing

13. When `config_.priority_aware` is true, `route()` distributes user
    messages across `lanes_.num_user_lanes()` based on `meta.priority`.
14. When false (default), all user messages go to lane 0.
15. `set_config()` calls `lanes_.set_num_user_lanes(config_.priority_levels)`.

Files touched: `mpsc_actor_mailbox.hpp`.

### Phase 3: `DropLowestPriority` handler

16. Create `drop_lowest_priority_handler.hpp`.
17. Extend `OverflowContext` with `MultiLaneQueue<T>* lanes`.
18. Wire into `make_overflow_handler`.

Files touched: `drop_lowest_priority_handler.hpp` (new),
`overflow_context.hpp`, `overflow_handler_factory.hpp`.

### Phase 4: Metrics and CLI

19. Add per-lane fields to `MboxSnapshot`.
20. Populate from `lanes_.lane_depth()` in `snapshot()`.
21. Add metric event types.
22. Update CLI rendering.

Files touched: `cli_types.hpp`, `mpsc_actor_mailbox.hpp`,
`metrics_event.hpp`, CLI inspect code.

### Phase 5: Configuration

23. Wire `priority_aware` and `priority_levels` through TOML parser
    and topology model to `MailboxConfig`.

Files touched: TOML actor parser, `topology_model.hpp`.

### Phase 6: Tests

24. `test_multi_lane_queue.cpp` — standalone unit tests for
    `MultiLaneQueue<T>`: enqueue/dequeue ordering, empty, lane depth,
    eviction, resize, reset.
25. `test_priority_lanes.cpp` — mailbox-level tests:
    - System lane isolation (full user lanes don't block system messages)
    - Priority routing (P0 before P1 before P2)
    - Default FIFO (priority_aware=false)
    - DropLowestPriority eviction
    - Per-lane snapshot
    - Configuration resizing
26. Update existing tests that depend on removed `ReservationManager`
    methods.

## 7. Test Plan

### 7.1 `test_multi_lane_queue.cpp` (standalone unit tests)

| Test | Description |
|------|-------------|
| `EnqueueDequeueSingleLane` | One user lane: FIFO order preserved |
| `EnqueueDequeueMultiLane` | Multi-lane: P0 drained before P1 |
| `SystemLanePriority` | System lane drained before any user lane |
| `EmptyAllLanes` | `empty()` true when all lanes empty |
| `EmptyWithSystemLane` | `empty()` false when system lane has data |
| `TryDropFromLowest` | Evicts from highest-index non-empty lane |
| `TryDropAllEmpty` | Returns false when all user lanes empty |
| `TotalDepth` | Sum across all lanes |
| `LaneDepth` | Per-lane count |
| `SetNumLanes` | Resize preserves existing lanes, truncates extras |
| `PendingFree` | set/get pending free lifecycle |
| `InjectForTest` | Direct injection without reservation |

### 7.2 `test_priority_lanes.cpp` (mailbox-level tests)

| Test | Description |
|------|-------------|
| `SystemMessageUsesSystemLane` | System messages go to system lane, bypass user capacity |
| `SystemLaneFullRejects` | Rejection when system lane at capacity |
| `SystemLaneIsolation` | User backlog doesn't delay system delivery |
| `PriorityAwareRouting` | User messages routed to correct lane |
| `PriorityAwareDequeueOrder` | P0 delivered before P1 before P2 |
| `DefaultFifoPreserved` | priority_aware=false: single lane FIFO |
| `DropLowestPriorityEviction` | Evicts from lowest-priority lane |
| `SnapshotPerLaneDepths` | MboxSnapshot shows per-lane depths |
| `ConfigResizeLanes` | Changing priority_levels takes effect |
| `BackwardsCompatAllExisting` | Existing test suite passes unchanged |

### 7.3 Existing test compatibility

Phase 0 is a pure refactor — all 22 existing mailbox test files pass
without changes. Phase 1 may require minor updates to tests that
directly observe `ReservationManager` internals (checking
`reserved_system_count()`). Phases 2-6 are additive.

## 8. Backwards Compatibility

| Concern | Resolution |
|---------|------------|
| `priority_aware = false` (default) | Identical behaviour |
| `MailboxConfig` layout | Append-only; no field reordering |
| `EnqueueResult` layout | Unchanged |
| `MPSCActorMailbox` public API | Unchanged |
| `ReservationManager` API | `try_reserve_system`/`reserved_system_count` removed (internal API) |
| ABI | Header-only; no ABI break |
| TOML config | New keys optional; existing configs parse unchanged |

## 9. Risk Assessment

| Risk | Mitigation |
|------|------------|
| Phase 0 refactor breaks existing behavior | Mechanical extraction — rename and delegate. Full test suite gate before proceeding. |
| Consumer lock hold time increases | Lane iteration: N+1 `count_.load()` calls per empty dequeue. N <= 8 -> < 20 cycles. |
| Memory overhead | N+1 `MPSCMailbox` stubs, ~128 bytes each. N=4 -> ~640 bytes/mailbox. |
| Starvation with `priority_aware = true` | Documented and intentional. Operators control priority assignment. |
| `DropLowestPriority` races with enqueue | Consumer lock serializes eviction scan; producers unaffected. |
| System lane rejection under control-plane storm | Bounded capacity is the safety valve. Operators size `protected_system_messages` to worst-case control-plane load. |
