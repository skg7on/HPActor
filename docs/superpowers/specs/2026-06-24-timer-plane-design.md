# TimerPlane: High-Performance Sharded Timer Subsystem

**Date:** 2026-06-24
**Issue:** #360 — feat(timer): Timer correctness and high-performance TimerPlane enhancement
**Status:** Design approved; implementation pending

## Overview

Two-phase enhancement of HPActor's timer subsystem:

1. **Phase 1: Bug fixes** — Five correctness fixes to existing `TimingWheel` and `CalendarQueue` backends, plus scheduler timer thread wakeup.
2. **Phase 2: TimerPlane** — New sharded, actor-aware `TimerBackend` variant with O(1) cancel, timer groups, MPSC command queues, memory-region allocation, and full observability.

Both backends coexist in `std::variant<TimingWheel, CalendarQueue, TimerPlane>`. TimerPlane is selected via `TimerBackend::TimerPlane` at scheduler construction.

## Phase 1: Bug Fixes

### Bug 1: `TimingWheel::insert_timer()` bucket-index from wrong time

**File:** `src/timer/timing_wheel.cpp:128`

**Root cause:** `insert_timer()` computes the bucket index from `(expire_ns - current_time_) >> level_shift`. When `current_time_` has advanced past the bucket boundary, near-term timers land in an already-processed bucket and wait a full rotation before firing.

**Fix:** Compute the bucket index from the expiry time directly: `(expire_ns >> level_shift) & mask`. Remove the `current_time_` offset. The test probes confirm: a 10ms timer should fire at ~10ms, not 20ms+.

**Regression risk:** Low. The bucket-index change is mechanical. Existing cascade logic and `advance()` rely on `expire_ns` for comparison, not bucket position, so cascading is unaffected. The `CascadePlacesTimerInCorrectBucket` test verifies round-trip correctness.

**Test:** Add `NearTermTimerFiresPromptly` — schedule a 5ms timer, advance in 1ms increments, assert callback fires by the 6th advance (tick_ns=1ms). Also add `BucketIndexUsesExpiryTime` — verify a future timer sharing the current bucket but not yet expired survives `advance()` to the boundary without firing.

### Bug 2: `CalendarQueue::advance()` drops future timers

**File:** `src/timer/calendar_queue.cpp:223`

**Root cause:** `advance()` in the fine wheel unlinks, erases, and destroys every timer in the current bucket without checking `expire_ns > now_ns`. Timers in the same fine bucket (1ms granularity) with expiry times after `now_ns` are destroyed prematurely.

**Fix:** Before destroying, check `timer->expire_ns <= now_ns`. Timers with future expiry are re-inserted into the wheel (they cascade naturally on the next advance cycle).

**Test:** Add `FutureTimerSurvivesAdvance` — schedule two timers at 0ms and 7ms, advance to 2ms, verify only the 0ms timer fired and the 7ms timer is still pending. Assert `size() == 1` and `next_deadline() != INT64_MAX`.

### Bug 3: `CalendarQueue` data races

**Files:** `src/timer/calendar_queue.cpp:107`, `include/hpactor/adt/calendar_queue.hpp:146`

**Root cause:**
- `schedule()` reads `last_advance_ns_` at line 107 before acquiring `mutex_`, while `advance()` writes it under `mutex_`.
- `size()` reads `timer_map_` at line 146 without acquiring `mutex_`.

**Fix:**
- Move `last_advance_ns_` read inside the mutex-locked section in `schedule()`.
- Make `size()` return an `std::atomic<size_t>` counter incremented/decremented on insert/cancel/expiry, or acquire the mutex. Use an atomic counter for zero-contention reads.

**Test:** Add TSAN-enabled stress test — concurrent `schedule()` and `advance()` from two threads, assert no TSAN warnings and no lost timers.

### Bug 4: Scheduler timer thread doesn't wake on new earlier deadline

**Files:** `src/sched/scheduler.cpp:98`, `src/sched/scheduler.cpp:323`

**Root cause:** `schedule_after()` inserts into the backend but does not wake the timer thread. If the thread is sleeping with `next_deadline()` far away or `INT64_MAX`, a new short timer can wait up to the 100ms sleep cap.

**Fix:** Add a `std::condition_variable` (or, for fd-based waiting compatible with `signalfd`, an `eventfd`) that the timer thread waits on with timeout. After inserting a timer whose deadline is earlier than the current sleep target, `schedule_after()` notifies the condition variable. The timer thread wakes, recomputes `next_deadline()`, and adjusts its sleep.

**Test:** Add `ScheduleWakesSleepingTimerThread` integration test — advance time, observe timer thread sleeping with a 100ms target, schedule a 1ms timer from the test thread, assert the callback fires within ~10ms (allowing for OS scheduling jitter).

### Bug 5: `TimingWheel::cancel()` is not O(1)

**Files:** `include/hpactor/timer/timing_wheel.hpp:28`, `src/timer/timing_wheel.cpp:147`

**Root cause:** The header advertises O(1) insert/cancel, but `remove_timer()` does a linear scan of all buckets at the encoded level.

**Fix:** Add a `std::unordered_map<uint64_t, Timer*>` (same approach CalendarQueue uses) for O(1) timer lookup by ID. On cancel, look up the Timer pointer directly, unlink from its bucket's `std::vector`, and erase from the map. On insert, add to the map. On expiry in `advance()`, remove from the map.

**Test:** Add `CancelIsConstantTime` — schedule N timers (N=1000), cancel the last one (worst case for linear scan), measure that cancel time is independent of N. Use a rough timing assertion (cancel < 10µs regardless of wheel size).

## Phase 2: TimerPlane

### Architecture

TimerPlane is a new `TimerBackend::TimerPlane` variant in `HybridScheduler`. It replaces the single-lock timer wheel with a sharded design:

```
TimerPlane
├── Shard[0..N-1]  (N = worker_count)
│   ├── Per-shard TimingWheel (fine-grained, 4-level hierarchical)
│   ├── MPSC command queue (lock-free bounded, for cross-thread ops)
│   ├── TimerNode* slot_array[] (pre-allocated, indexed by slot for O(1) access)
│   ├── std::vector<uint32_t> free_slots (LIFO reuse)
│   ├── std::atomic<int64_t> min_deadline (lock-free read by timer thread)
│   ├── std::atomic<uint64_t> metrics[pending|scheduled|fired|cancelled|late|dropped]
│   └── std::mutex (serializes wheel + slot ops on this shard only)
├── Timer thread (polls all shard min_deadlines, advances each independently)
├── eventfd wakeup (interrupts timer thread sleep — fixes bug #4)
├── TimerGroup registry (actor_id → TimerGroup)
├── MpscRingBuffer<MetricEvent>* metrics_ring_buffer
└── SlabCache integration for TimerNode allocation
```

### Shard Selection

```cpp
uint32_t shard_index = hash(thread_id) % num_shards;
```

- Actors on the same worker naturally use the same shard for self-scheduled timers (no cross-thread traffic).
- Cross-thread schedules go through the target shard's MPSC command queue.
- Schedule-to-remote-actor timers use hash(target_actor_id) for shard selection.

### TimerHandle Encoding (64-bit)

```
Bits [0..23]:   slot_index    (16M timers per shard)
Bits [24..31]:  generation    (0-255, ABA protection)
Bits [32..47]:  shard_index   (up to 65536 shards)
Bits [48..63]:  type_tag      (0=actor_msg, 1=generic_cb, 2=io_timeout, etc.)
```

Slot lookup is direct: `shard.slot_array[slot_index]`. Generation check prevents ABA reuse bugs. Freed slots go on a per-shard LIFO free list.

Helper constructor:
```cpp
static TimerHandle make_handle(uint32_t shard, uint32_t slot,
                                uint8_t gen, uint16_t type);
```

Decode helpers:
```cpp
uint32_t slot_index(TimerHandle h);
uint32_t shard_index(TimerHandle h);
uint8_t generation(TimerHandle h);
```

### Intrusive TimerNode

```cpp
struct TimerNode : SlabAllocated<TimerNode> {
    TimerNode* next{nullptr};
    TimerNode* prev{nullptr};
    int64_t expire_ns{0};
    uint32_t slot_index{0};
    uint8_t generation{0};
    TimerGroupHandle group_handle{0};
    uint8_t priority{0};
    TraceContext trace;
    TimerCallback callback;     // std::function<void()> for generic callbacks
    TypedMessage* actor_msg{nullptr}; // for actor-message timers (owned)
};
```

- Doubly-linked for O(1) unlink on cancel.
- `callback` is used for generic `call_after()`. For actor-message timers (`schedule_self`, `schedule_to`), `actor_msg` is set and the callback is auto-generated to deliver the message.
- Allocation from `mem::allocate()` with memory region `kTimer`.

### MPSC Command Queue (per shard)

Lock-free bounded queue for cross-thread operations. Pre-allocated capacity of 256 commands per shard.

```cpp
struct TimerCommand {
    enum Type : uint8_t { Schedule, Cancel, DrainGroup, Shutdown };
    Type type;
    union {
        struct { int64_t expire_ns; TimerNode* node; } schedule;
        struct { uint64_t handle; } cancel;
        struct { TimerGroupHandle group; } drain;
    };
};

class TimerCommandQueue {
    std::array<TimerCommand, 256> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
public:
    bool try_push(TimerCommand cmd);   // returns false if full
    bool try_pop(TimerCommand& cmd);   // returns false if empty
    size_t drain_all(std::vector<TimerCommand>& out); // batch drain
};
```

If `try_push()` fails (queue full), the caller falls back to acquiring the shard mutex directly and performing the operation inline.

### TimerGroup

Keyed by `ActorId` for implicit actor groups, with optional named subgroups for subsystems:

```cpp
class TimerGroup {
    std::unordered_set<uint64_t> handles_;
public:
    void add(uint64_t handle);
    void remove(uint64_t handle);     // on cancel or fire
    size_t cancel_all(TimerPlane&);   // bulk cancel, returns count
    size_t size() const;
    bool empty() const;
};

using TimerGroupHandle = uint64_t;  // encodes (actor_id, subgroup_index)
```

TimerPlane holds `std::unordered_map<ActorId, TimerGroup> groups_` protected by a reader-writer lock (read-heavy: timers added/removed; write-rare: group creation/deletion, actor stop). Bulk cancel on actor stop/passivation calls `cancel_all()` for the actor's group without iterating all shards — each shard's bucket nodes reference their group, and the group's handle set provides the lookup keys.

### Timer Thread

Single thread, replaces current timer thread:

```cpp
void TimerPlane::timer_loop() {
    while (running_.load(std::memory_order_acquire)) {
        int64_t now = monotonic_now_ns();
        int64_t next_deadline = INT64_MAX;

        for (auto& shard : shards_) {
            shard.drain_command_queue();       // process cross-thread commands
            shard.advance_wheel(now);          // fire expired timers, emit metrics
            next_deadline = std::min(next_deadline,
                shard.min_deadline.load(std::memory_order_acquire));
        }

        if (next_deadline == INT64_MAX) {
            // No pending timers: sleep until woken
            wait_on_eventfd(wakeup_fd_, 100'000'000); // 100ms max
        } else {
            int64_t sleep_ns = std::clamp(next_deadline - monotonic_now_ns(),
                                          1'000'000LL, 100'000'000LL);
            wait_on_eventfd(wakeup_fd_, sleep_ns);
        }
    }
}
```

### API: ActorContext extensions

```cpp
// TimerOptions — new type in include/hpactor/timer/timer_options.hpp
struct TimerOptions {
    uint8_t priority{0};              // 0=highest, 255=lowest
    std::chrono::milliseconds deadline{0}; // 0 = use expire time
    DeliveryMode delivery_mode{DeliveryMode::BestEffort};
    bool propagate_trace{true};
    uint64_t tolerance_ns{0};         // coalescing tolerance

    // Factory for defaults
    static TimerOptions defaults();
    static TimerOptions high_priority();
};

// ActorContext — extended signatures (backward compatible defaults)
AlarmHandle schedule(std::chrono::milliseconds delay, TypedMessage msg,
                     TimerOptions opts = {});
AlarmHandle schedule_to(const ActorAddress& target,
                        std::chrono::milliseconds delay, TypedMessage msg,
                        TimerOptions opts = {});

// Timer group accessor (new)
TimerGroupHandle timer_group() const;         // actor's implicit group
TimerGroupHandle create_timer_group(std::string_view name);
void cancel_timer_group(TimerGroupHandle h);  // bulk cancel subgroup
```

### API: IScheduler extensions

```cpp
// Generic callback (existing, unchanged)
TimerHandle schedule_after(timer_callback cb, int64_t delay_ns);

// New: schedule with options
TimerHandle schedule_after(timer_callback cb, int64_t delay_ns,
                           TimerOptions opts);
```

### State Machine

```
TimerNode lifecycle:
  FREE → ALLOCATED → QUEUED (in wheel bucket)
                   → CANCELLED (removed from wheel, returned to free list)
  QUEUED  → FIRING (callback executing on some thread)
          → CANCELLED
  FIRING  → FIRED (callback completed successfully)
          → FAILED (callback threw; node freed, metrics incremented)
  FIRED   → FREE (node returned to pool)
  FAILED  → FREE
  CANCELLED → FREE
```

## Observability

### MetricEvent Types (new)

| # | Type | Kind | Description |
|---|------|------|-------------|
| 27 | `kTimerScheduled` | Counter | Timer scheduled (per-shard, per-type_tag) |
| 28 | `kTimerFired` | Counter | Timer fired on time |
| 29 | `kTimerCancelled` | Counter | Timer explicitly cancelled |
| 30 | `kTimerLate` | Counter | Timer fired >1 tick past expiry |
| 31 | `kTimerDropped` | Counter | Timer dropped (queue full, alloc failure) |
| 32 | `kTimerFiringLatency` | Histogram | Firing lateness in µs |
| 33 | `kTimerCallbackDuration` | Histogram | Callback wall-clock duration in µs |

### Timer-specific Metrics (OpenMetrics exposition)

```
# HELP hpactor_timer_pending Current pending timers
# TYPE hpactor_timer_pending gauge
hpactor_timer_pending{shard="0"} 42
hpactor_timer_pending{shard="1"} 17

# HELP hpactor_timer_fired_total Total timers fired
# TYPE hpactor_timer_fired_total counter
hpactor_timer_fired_total{shard="0"} 10423

# HELP hpactor_timer_late_total Timers fired past expiry
# TYPE hpactor_timer_late_total counter
hpactor_timer_late_total{shard="0"} 3

# HELP hpactor_timer_cmd_queue_depth MPSC command queue depth
# TYPE hpactor_timer_cmd_queue_depth gauge
hpactor_timer_cmd_queue_depth{shard="0"} 0

# HELP hpactor_timer_alloc_failures_total Timer node allocation failures
# TYPE hpactor_timer_alloc_failures_total counter
hpactor_timer_alloc_failures_total 0

# HELP hpactor_timer_firing_latency_us Firing lateness distribution
# TYPE hpactor_timer_firing_latency_us histogram
hpactor_timer_firing_latency_us{le="100"} 9500
hpactor_timer_firing_latency_us{le="500"} 9800
hpactor_timer_firing_latency_us{le="1000"} 9950
hpactor_timer_firing_latency_us_bucket{le="+Inf"} 10000
```

### CLI Commands

| Command | Description |
|---------|-------------|
| `/timer stats` | Aggregate: total pending/fired/cancelled/late across all shards, next deadline |
| `/timer stats <shard>` | Per-shard: pending, cmd queue depth, fired/late/dropped counters |
| `/timer inspect <handle>` | Resolve a handle: shard, slot, gen, expire, group, type |
| `/timer groups` | List all timer groups: actor_id, name, count |
| `/timer groups <actor_id>` | Detail for one actor: all pending handles, expire times |

Implementation: `src/cli/commands/timer_commands.cpp`, self-registering into the existing command tree.

## Testing Strategy

### Phase 1 Tests

| Test | Tier | File | Description |
|------|------|------|-------------|
| `NearTermTimerFiresPromptly` | unit | `test_timing_wheel.cpp` | 5ms timer fires by 6th 1ms advance — validates Bug 1 fix |
| `BucketIndexUsesExpiryTime` | unit | `test_timing_wheel.cpp` | Timer in current bucket with future expiry not fired early |
| `FutureTimerSurvivesAdvance` | unit | `test_calendar_queue.cpp` (new) | 7ms timer survives advance(2ms) — validates Bug 2 fix |
| `ScheduleReadsTimeUnderLock` | unit | `test_calendar_queue.cpp` | TSAN validation of schedule/advance/size — validates Bug 3 fix |
| `ScheduleWakesSleepingTimerThread` | integration | `test_timer_wakeup.cpp` (new) | Short timer wakes thread from long sleep — validates Bug 4 fix |
| `CancelIsConstantTime` | unit | `test_timing_wheel.cpp` | 1000-timer cancel performance — validates Bug 5 fix |
| `CancelO1UsesMap` | unit | `test_timing_wheel.cpp` | Cancel via map lookup, verify timer unlinked from bucket |

### Phase 2 Tests

| Test | Tier | File | Description |
|------|------|------|-------------|
| `ShardSelectionDeterminism` | unit | `test_timer_plane.cpp` (new) | Same thread → same shard |
| `TimerHandleEncodeDecode` | unit | `test_timer_plane.cpp` | Round-trip: shard/slot/gen/type ↔ handle |
| `SlotAllocReuseGeneration` | unit | `test_timer_plane.cpp` | Alloc, free, re-alloc: gen incremented, ABA prevented |
| `InsertFireExpired` | unit | `test_timer_plane.cpp` | Schedule timer, advance past expiry, assert fired |
| `CancelPreventsFire` | unit | `test_timer_plane.cpp` | Schedule, cancel, advance — no fire |
| `BulkCancelTimerGroup` | unit | `test_timer_plane.cpp` | 10 timers in group, cancel_all, verify 0 fires |
| `MpscQueuePushDrain` | unit | `test_timer_plane.cpp` | Push command, drain, verify operation performed |
| `TimerNodeAllocFromRegion` | unit | `test_timer_plane.cpp` | Allocate/free TimerNode from `mem::allocate(kTimer)` |
| `FaultInjectionHooks` | unit | `test_timer_plane.cpp` | FAULT_INJECT for alloc fail, queue full, late fire |
| `ActorScheduleThroughTimerPlane` | integration | `test_timer_plane_actor.cpp` (new) | Actor schedules, message delivered via TimerPlane |
| `CancelPreventsActorDelivery` | integration | `test_timer_plane_actor.cpp` | Cancel after schedule, verify no message in mailbox |
| `TimerGroupCancelOnActorStop` | integration | `test_timer_plane_actor.cpp` | Actor stop → pending timers cancelled via group |
| `CrossThreadScheduleWorks` | integration | `test_timer_plane_actor.cpp` | Schedule from thread A, deliver to actor on thread B |
| `LateFiringMetricEmitted` | integration | `test_timer_plane_actor.cpp` | Timer fires late, verify kTimerLate metric |
| `TimerPlaneBackendEndToEnd` | system | `test_timer_plane_system.cpp` (new) | Full system with TimerPlane as active backend |
| `GracefulShutdownPendingTimers` | system | `test_timer_plane_system.cpp` | Shutdown with pending timers, group cancel invoked |
| `MetricsEndpointReturnsTimerMetrics` | system | `test_timer_plane_system.cpp` | Scrape /metrics, verify timer gauges/counters present |

## Implementation Phases

1. **Bug fixes** (Phase 1) — 5 standalone fixes, each with RED→GREEN→REFACTOR cycle
2. **TimerPlane core** — Shard, TimerNode, TimerHandle encoding, slot array, MPSC queue
3. **TimerPlane backend integration** — `TimerBackend::TimerPlane` variant, timer thread, `schedule`/`cancel`/`advance`
4. **TimerGroup + ActorContext APIs** — `TimerOptions`, `TimerGroup`, bulk cancel, `timer_group()` accessor
5. **Metrics + CLI** — `MpscRingBuffer` integration, `/timer` commands
6. **Integration + system tests** — Full-stack verification with TimerPlane as active backend

## Key Design Decisions

- **TimerPlane is a new `TimerBackend` variant**, not a wrapper or standalone subsystem. It replaces the timer wheel inside the scheduler, reusing the scheduler thread and `schedule_after`/`cancel_timer` dispatch.
- **Extend existing `ActorContext::schedule()` API** with optional `TimerOptions` rather than introducing a new `timer_plane()` accessor on ActorContext. Backward compatible.
- **TimerHandle encodes shard+slot+generation** in 64 bits for direct O(1) lookup without any map or hash table. CalendarQueue's existing `unordered_map` approach is preserved in the existing backends but not used in TimerPlane.
- **`std::function<void()>` callback for generic timers, `TypedMessage*` for actor messages** — avoids capturing `shared_ptr<TypedMessage>` in `std::function` (the current workaround) while keeping the generic callback path available.
- **One timer thread**, not N per-shard threads. A single thread polling all shards is simpler, uses less memory, and the shard mutex contention is minimal (advance is O(fired_count), not O(pending_count)).

## Files to Create

| File | Purpose |
|------|---------|
| `include/hpactor/timer/timer_plane.hpp` | TimerPlane class definition |
| `include/hpactor/timer/timer_node.hpp` | Intrusive TimerNode + TimerCommand types |
| `include/hpactor/timer/timer_options.hpp` | TimerOptions struct |
| `include/hpactor/timer/timer_group.hpp` | TimerGroup class |
| `include/hpactor/timer/timer_plane_metrics.hpp` | Timer metric event type extensions |
| `src/timer/timer_plane.cpp` | TimerPlane implementation |
| `src/timer/timer_plane_shard.cpp` | Per-shard wheel advance, command processing |
| `src/timer/timer_group.cpp` | TimerGroup implementation |
| `src/cli/commands/timer_commands.cpp` | CLI `/timer` commands |
| `tests/unit/timer/test_calendar_queue.cpp` | CalendarQueue unit tests (Bug 2, Bug 3) |
| `tests/unit/timer/test_timer_plane.cpp` | TimerPlane unit tests |
| `tests/integration/actor/test_timer_wakeup.cpp` | Bug 4 integration test |
| `tests/integration/actor/test_timer_plane_actor.cpp` | TimerPlane actor integration |
| `tests/system/test_timer_plane_system.cpp` | TimerPlane system tests |

## Files to Modify

| File | Change |
|------|--------|
| `src/timer/timing_wheel.cpp` | Bug 1 fix: bucket-index from expiry; Bug 5 fix: add timer_map_ |
| `include/hpactor/timer/timing_wheel.hpp` | Bug 5 fix: add timer_map_ member |
| `src/timer/calendar_queue.cpp` | Bug 2 fix: check expire before destroy; Bug 3 fix: lock around last_advance_ns_ |
| `include/hpactor/adt/calendar_queue.hpp` | Bug 3 fix: atomic size counter |
| `src/sched/scheduler.cpp` | Bug 4 fix: condition variable wakeup; TimerPlane variant support |
| `include/hpactor/sched/scheduler.hpp` | Add `TimerBackend::TimerPlane`, variant member |
| `include/hpactor/sched/scheduler_interfaces.hpp` | TimerHandle encode/decode helpers |
| `include/hpactor/actor/actor_context.hpp` | TimerOptions parameter on schedule/schedule_to |
| `src/actor/actor_context.cpp` | TimerOptions propagation, timer_group accessor |
| `include/hpactor/types/types.hpp` | AlarmHandle ↔ TimerHandle conversion for TimerPlane |
| `include/hpactor/metrics/metrics_event.hpp` | New timer metric event types |

## Build Verification

1. `ninja -C build` — compile all changes
2. `ctest -R "TimingWheel|CalendarQueue|TimerPlane|timer_wakeup" --output-on-failure` — run all timer tests
3. `./build/tests/unit/timer/test_unit_timer --gtest_filter="*"` — verify no regressions
4. Full `ctest --output-on-failure --parallel 8` — verify existing 2105+ tests still pass
