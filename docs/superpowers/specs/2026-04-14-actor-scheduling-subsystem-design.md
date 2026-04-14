# Actor Scheduling Subsystem — Implementation Design

> **Supersedes:** `src/actor/scheduler.cpp` (simple mutex-based scheduler)
> **Design basis:** `docs/architecture/scheduling/scheduling-subsystem-design.md`, `scheduling-mathematical-model.md`, `scheduling-architecture-design.md`
> **Approach:** Priority-first incremental rollout (Option C + A)

---

## Phase 1: Chase-Lev Deques + Multi-Priority Work Queue

### Purpose

Replace the mutex-protected `std::vector` in `Scheduler::work_queue_` with per-worker lock-free Chase-Lev deques organized by priority. This gives O(1) owner push/pop and O(1) lock-free stealing, eliminating the global mutex bottleneck.

### Files

| File | Role |
| :---- | :---- |
| `include/hpactor/sched/work_queue.hpp` | Public headers: `ChaselevDeque<T>`, `WorkItem`, `MultiPriorityWorkQueue` |
| `src/sched/work_queue.cpp` | Implementation of `ChaselevDeque` growth and any shared logic |

### `WorkItem` Struct

```cpp
struct WorkItem {
    ActorId  actor;
    int64_t  deadline_ns;   // absolute CLOCK_MONOTONIC; INT64_MAX if none
    uint64_t sequence;      // FIFO tiebreaker within same deadline
};
```

`ActorId` comes from `include/hpactor/types/types.hpp` — no new dependencies introduced.

### `ChaselevDeque<T>` Design

A dynamic circular array deque. Key operations:

| Operation | Caller | Complexity | Safety |
| :-------- | :----- | :--------- | :----- |
| `push_bottom(T item)` | Owner thread only | O(1) amortized | Wait-free |
| `pop_bottom(T& out)` | Owner thread only | O(1) amortized | Wait-free |
| `steal_top(T& out)` | Any thief thread | O(1) | Lock-free, ABA-safe via version tag |
| `size_approx()` | Any thread | O(1) | Relaxed atomic |

ABA safety: each CAS on `top_` uses a `(new_top, old_tag+1)` pair. The tag increment invalidates any interrupted CAS that later retries with the same old `top` value.

Growth: when `bottom - top > capacity`, allocate a new `CircularArray` at 2× capacity, copy valid items, and atomically swap `array_`. Old arrays are retained in `garbage_` for reclamation.

### `MultiPriorityWorkQueue` Design

Array of P `ChaselevDeque<WorkItem>` (default P = 4). Priority 0 = highest.

```cpp
class MultiPriorityWorkQueue {
public:
    explicit MultiPriorityWorkQueue(uint32_t priority_levels = 4);

    void push(uint8_t priority, WorkItem item);   // owner only
    bool pop(WorkItem& out);                       // owner only: scans high→low
    bool steal(uint8_t priority, WorkItem& out);  // thief only
    size_t depth_approx() const;                  // sum across all levels
};
```

### Build Integration

- New directory `src/sched/` created alongside existing `src/actor/`
- `CMakeLists.txt` updated to add `src/sched/work_queue.cpp` to `hpactor_lib`
- Existing `src/actor/scheduler.cpp` remains unchanged until Phase 2 integration

---

## Phase 2: Worker Thread + IScheduler Interface

### Purpose

Define the abstract `IScheduler` interface, implement `WorkerThread` and `HybridScheduler`, and wire into `ActorSystem`. The existing `Scheduler` class is retained as a fallback behind a compile-time flag until this phase is verified.

### Files

| File | Role |
| :---- | :---- |
| `include/hpactor/sched/scheduler.hpp` | `IScheduler`, `HybridScheduler`, `TimerHandle` |
| `include/hpactor/sched/worker_thread.hpp` | `WorkerThread` |
| `src/sched/scheduler.cpp` | `HybridScheduler` methods |
| `src/sched/worker_thread.cpp` | `WorkerThread::run_loop()` |

### `IScheduler` Interface

```cpp
class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;
    virtual void notify_idle(ActorId actor) = 0;
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;
    virtual TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) = 0;
    virtual void cancel_timer(TimerHandle handle) = 0;
    virtual size_t worker_count() const = 0;

    using timer_callback = std::function<void()>;
};
```

### `WorkerThread` Design

Each worker owns:
- `MultiPriorityWorkQueue queues_` — local P-level deques
- `CoroutineFramePool frame_pool_` — thread-local slab (Phase 5 slot, initialized empty for now)
- `std::thread thread_` — the OS thread
- `alignas(64) std::atomic<uint32_t> load_` — approximate queue depth for A2WS

`run_loop()`:
```
while (!stop_) {
    if (!try_execute_one()) {   // pop from local queues
        // Phase 3: try_steal_work() via A2WS
        std::this_thread::yield();
    }
}
```

### `HybridScheduler` Design

```cpp
class HybridScheduler final : public IScheduler {
public:
    struct Config {
        size_t   num_workers      = std::thread::hardware_concurrency();
        uint32_t priority_levels   = 4;
        uint32_t steal_threshold   = 2;   // τ: Phase 3
        uint32_t timer_resolution_us = 1000;  // Phase 4
    };

    explicit HybridScheduler(ActorSystem& system, Config cfg = {});
    // ... IScheduler interface ...

private:
    size_t select_worker(uint8_t priority) const;  // round-robin per priority

    ActorSystem&                  system_;
    Config                        cfg_;
    std::vector<WorkerThread>      workers_;
    std::atomic<bool>              running_{false};
};
```

`notify_ready()`: calls `select_worker(priority)` → `workers_[i].push(WorkItem{actor, deadline, seq++})`. For Phase 2, `EDFPriorityQueue` and `A2WSCoordinator` are stubs (no-ops). Routing is round-robin only.

### `TimerHandle`

```cpp
struct TimerHandle {
    uint64_t id = 0;
    bool valid() const noexcept { return id != 0; }
};
```

For Phase 2, `schedule_after`/`schedule_every`/`cancel_timer` are stub implementations returning `TimerHandle{0}` / no-ops. This lets `HybridScheduler` compile before Phase 4.

### ActorSystem Integration

- `ActorSystem` holds `std::unique_ptr<IScheduler>` instead of `std::unique_ptr<Scheduler>`
- New `USE_HYBRID_SCHEDULER` CMake option (default OFF) controls which is instantiated
- Until Phase 3, `HybridScheduler` is functionally equivalent to the old `Scheduler` but with per-worker deques and no global mutex on the hot path

---

## Phase 3: EDF + A2WS Load Balancing

### Purpose

Add global EDF index and adaptive work-stealing so that idle workers steal from loaded ones based on priority and deadline, not just round-robin.

### Files

| File | Role |
| :---- | :---- |
| `include/hpactor/sched/edf_queue.hpp` | `EDFPriorityQueue` |
| `include/hpactor/sched/a2ws.hpp` | `A2WSCoordinator`, `LoadSnapshot` |
| `src/sched/edf_queue.cpp` | `EDFPriorityQueue` spinlocked binary min-heap |
| `src/sched/a2ws.cpp` | `A2WSCoordinator` snapshot ring + `best_victim()` |

### `EDFPriorityQueue` Design

Binary min-heap keyed by `(priority ASC, deadline_ns ASC)`. Thread-safe via brief spinlock (not a long hold — only upsert/remove are locked).

```cpp
void upsert(EDFEntry entry);     // insert or update by ActorId
void remove(ActorId actor);      // called on notify_idle
bool pop_min(EDFEntry& out);     // for debugging/diagnostics
```

### `A2WSCoordinator` Design

Ring topology: worker i's neighbors are `(i-1) % m` and `(i+1) % m`. Each `Snapshot` is `alignas(64)` to prevent false sharing.

```cpp
void update_snapshot(size_t worker_index, uint32_t load) noexcept;
size_t best_victim(size_t requester, uint8_t priority) const noexcept;
uint32_t steal_count(uint32_t own_load, uint32_t victim_load) const noexcept;
```

For Phase 3, `best_victim()` uses the ring neighbor check. Stealing from the chosen victim calls `WorkerThread::try_steal()`.

### `HybridScheduler::select_worker()` Upgrade

Phase 2: round-robin only.

Phase 3: reads A2WS snapshots, picks the least-loaded worker. On tie, round-robin within the minimum-load group.

---

## Phase 4: Hierarchical Timing Wheel

### Purpose

Replace any ad-hoc timer mechanisms with a layered timing wheel providing O(1) insert/cancel amortized.

### Files

| File | Role |
| :---- | :---- |
| `include/hpactor/timer/timing_wheel.hpp` | `HierarchicalTimingWheel`, `TimerEntry`, `TimerHandle` |
| `src/timer/timing_wheel.cpp` | Wheel logic, cascade, advance |

### `HierarchicalTimingWheel` Design

4 levels (configurable):
| Level | Resolution | Slots | Span |
| :---- | :--------- | :---- | :--- |
| 0 | 1 ms | 256 | 256 ms |
| 1 | 256 ms | 64 | ~16 s |
| 2 | ~16 s | 64 | ~17 min |
| 3 | ~17 min | 64 | ~18 hr |

All timer operations (`insert_after`, `insert_every`, `cancel`, `advance`) run on a single dedicated timer thread owned by `HybridScheduler`. `IScheduler::schedule_after` / `schedule_every` / `cancel_timer` delegate to `HierarchicalTimingWheel`.

`TimerEntry` is an intrusive node embedded in caller's structure (zero allocation):

```cpp
struct TimerEntry {
    TimerHandle    handle;
    int64_t        expires_at_ns;
    int64_t        interval_ns;   // 0 = one-shot
    timer_callback cb;
    TimerEntry*    next = nullptr;
};
```

---

## Phase 5: Coroutine Frame Pool

### Purpose

Thread-local slab allocator for C++20 stackless coroutine frames, intercepting `promise_type::operator new` to eliminate per-allocation syscalls.

### Files

| File | Role |
| :---- | :---- |
| `include/hpactor/memory/coroutine_frame_pool.hpp` | `CoroutineFramePool` |
| `src/memory/coroutine_frame_pool.cpp` | Implementation |

### Design

- `CoroutineFramePool` is a data member of `WorkerThread`, thread-local by construction
- Default: 256 slots × 512 bytes = 128 KB per worker thread
- Allocation: O(1) bitmap TZCNT scan
- Fallback: `::operator new` if slot exhausted or `size > slot_size`
- `WorkerThread::frame_pool()` exposes the pool; `promise_type::operator new` calls `WorkerThread::frame_pool().allocate(size)` via thread-local pointer

---

## Directory Structure After All Phases

```
include/hpactor/
├── sched/
│   ├── scheduler.hpp          # IScheduler + HybridScheduler
│   ├── worker_thread.hpp      # WorkerThread
│   ├── work_queue.hpp         # ChaselevDeque, MultiPriorityWorkQueue, WorkItem
│   ├── edf_queue.hpp          # EDFPriorityQueue
│   └── a2ws.hpp               # A2WSCoordinator
├── timer/
│   ├── timing_wheel.hpp        # HierarchicalTimingWheel
│   └── timer_entry.hpp        # TimerEntry
└── memory/
    └── coroutine_frame_pool.hpp # CoroutineFramePool

src/
├── sched/
│   ├── scheduler.cpp          # HybridScheduler
│   ├── worker_thread.cpp      # WorkerThread
│   ├── work_queue.cpp         # ChaselevDeque growth
│   ├── edf_queue.cpp          # EDFPriorityQueue
│   └── a2ws.cpp               # A2WSCoordinator
├── timer/
│   └── timing_wheel.cpp        # HierarchicalTimingWheel
└── memory/
    └── coroutine_frame_pool.cpp
```

---

## Testing Strategy

| Phase | Tests |
| :---- | :---- |
| 1 | Unit test for `ChaselevDeque`: concurrent push_bottom/steal_top, verify no loss or duplication; `MultiPriorityWorkQueue` priority ordering |
| 2 | Integration: spawn actors, send messages, verify delivery; worker count config |
| 3 | EDF ordering: enqueue actors with different deadlines, verify earliest-deadline-first processing; A2WS: two workers, steal from loaded to empty |
| 4 | Timer: schedule_after fires within 1ms of deadline; cancel returns no-op; repeating fires at correct interval |
| 5 | Coroutine: actor uses `co_await`, verify frame allocated from pool, not heap |

All tests run under `ctest --output-on-failure`.

---

## Compilation Flags

- `-fno-exceptions`, `-fno-rtti` (already in `CMakeLists.txt`)
- No changes to compiler flags required
- TSAN/ASAN via `cmake -DENABLE_TSAN=ON` / `-DENABLE_ASAN=ON`

---

## Scope Boundaries (Hard Rules)

1. `::sched` may use `::timer`, `::memory`, and `::types` — but never `::actor` or `::net`
2. `HybridScheduler` holds `ActorSystem&` as the single controlled seam (interface uses only `ActorId`)
3. `ActorSystem` sits at the top of the dependency DAG and may use everything
4. The `IScheduler` interface uses only `ActorId` (a `::types` concept) — no actor or net types leak in
