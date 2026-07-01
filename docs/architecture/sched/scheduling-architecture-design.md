## Actor System with Hybrid Coroutine Scheduling — Detailed Architecture Design

> **See also:** [Mathematical Model and Formal Definitions](scheduling-mathematical-model.md)

This document specifies the concrete module boundaries, class hierarchy, C++ API contracts, and inter-component data flow that translate the theoretical foundations into an implementable system.

---

### Module and Sub-module Boundaries

#### Directory and Namespace Layout

The scheduling subsystem occupies the `hpactor::sched` namespace. All scheduling code lives under `include/hpactor/sched/` (public headers) and `src/sched/` (implementations). Adjacent subsystems used by the scheduler — mailbox, timer, and memory — have their own namespaces and are treated as **dependencies, not peers**.

```
include/hpactor/
├── sched/
│   ├── scheduler.hpp            # IScheduler + HybridScheduler declaration
│   ├── worker_thread.hpp        # WorkerThread
│   ├── work_queue.hpp           # ChaselevDeque<T>, MultiPriorityWorkQueue
│   ├── edf_queue.hpp            # EDFPriorityQueue
│   └── a2ws.hpp                 # A2WSCoordinator, LoadSnapshot
├── mailbox/
│   ├── mpsc_mailbox.hpp         # MPSCMailbox<T>
│   └── spsc_channel.hpp         # SPSCChannel<T>  (high-throughput pair comms)
├── timer/
│   ├── timing_wheel.hpp         # HierarchicalTimingWheel, TimerHandle
│   └── timer_entry.hpp          # TimerEntry (intrusive node)
└── memory/
    └── coroutine_frame_pool.hpp # CoroutineFramePool (thread-local slab)

src/
├── sched/
│   ├── scheduler.cpp
│   ├── worker_thread.cpp
│   └── a2ws.cpp
├── timer/
│   └── timing_wheel.cpp
└── memory/
    └── coroutine_frame_pool.cpp
```

#### Module Dependency Graph

Arrows denote "may include headers from." No cycles are permitted.

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  hpactor     │────▶│  hpactor     │────▶│  hpactor     │
│  ::actor     │     │  ::sched     │     │  ::mailbox   │
└──────────────┘     └──────┬───────┘     └──────────────┘
                            │                      ▲
                            ▼                      │
                     ┌──────────────┐     ┌──────────────┐
                     │  hpactor     │     │  hpactor     │
                     │  ::timer     │     │  ::types     │◀─── all modules
                     └──────────────┘     └──────────────┘
                            ▲
                     ┌──────────────┐
                     │  hpactor     │
                     │  ::memory    │
                     └──────────────┘
```

**Hard rules:**
1. `::timer` and `::memory` have no upward dependencies — they are pure utilities.
2. `::mailbox` depends only on `::types` — it has no knowledge of actors or the scheduler.
3. `::sched` may use `::mailbox`, `::timer`, `::memory`, and `::types`, but never `::actor` or `::net`.
4. `::actor` sits at the top and may use everything.
5. `::net` (EventLoop, I/O backends) is a sibling of `::sched`; neither depends on the other. Wiring happens in `::actor`.

**Clarification on `ActorId` and `ActorSystem&`:**
- `ActorId` is defined in `::types` (`include/hpactor/types/types.hpp`), so `::sched` using `ActorId` does **not** violate rule 3.
- `HybridScheduler` holds `ActorSystem& system_` as a deliberate architectural coupling point: the concrete scheduler must look up actor mailboxes and deliver completions, which requires `ActorSystem`. This coupling is confined to the concrete class, not the `IScheduler` interface. The interface itself uses only `ActorId` (a `::types` concept), keeping the abstract contract dependency-clean. The `ActorSystem&` parameter in `HybridScheduler`'s constructor is the single controlled seam where `::sched` reaches into `::actor`.

---

### Class Hierarchy

#### UML Overview

```
«interface»                        «interface»
IScheduler                         IMailbox<T>
    │                                  │
    └── HybridScheduler         ┌──────┴──────────────┐
            │                   │                     │
            ├─◆ WorkerThread[m] MPSCMailbox<T>    MultiLaneQueue<T>
            │       │           (adt/mpsc_mailbox) (mailbox/multi_lane_queue)
            │       │                                   │
            │       ├─◆ MultiPriorityWorkQueue          ├─◆ MPSCMailbox<T>[1+N]
            │       │       │                           │   (1 system + N user lanes)
            │       │       └─◆ ChaselevDeque<WorkItem>[P]  │
            │       │                                   └─◆ pending_free_ring_[8]
            │       ├─◆ CoroutineFramePool              (deferred-free ring buffer)
            │       │   (thread_local)
            │       └─◆ backoff_counter_ (atomic)
            │           adaptive idle: yield→sleep→CV
            │
            ├─◆ ActorExecutionEngine
            │       │
            │       ├─◆ BehaviorActorRunner  (event-based actors)
            │       └─◆ CoroutineActorRunner (coroutine actors)
            │
            ├─◆ ActorReadyGate
            │       │
            │       ├─◆ try_mark_ready()         (external notification)
            │       ├─◆ mark_ready_already_admitted()  (RequeueReady fast path)
            │       └─◆ is_ready() / set_ready() / set_idle()
            │
            ├─◆ EDFPriorityQueue
            │       │
            │       └─◆ std::vector<EDFEntry>  (binary min-heap)
            │
            ├─◆ A2WSCoordinator
            │       │
            │       └─◆ LoadSnapshot[m]  (ring array)
            │
            └─◆ HierarchicalTimingWheel
                    │
                    └─◆ TimerWheel[W]
                            │
                            └─◆ TimerSlot[S]  (intrusive linked list head)
```

**Legend:**
- `─◆` = composition (owner controls lifetime)
- `──▷` = inheritance / interface implementation
- `[n]` = fixed-size array of n elements
- `[1+N]` = 1 system lane + N user lanes

#### Key Design Decisions

| Decision | Rationale |
| :------- | :-------- |
| `IScheduler` is an abstract interface | Enables test doubles and future scheduler variants (e.g., single-threaded for unit tests) |
| `WorkerThread` owns its `CoroutineFramePool` | Pool is thread-local; ownership in `WorkerThread` makes the association explicit and ensures cleanup on thread exit |
| `HierarchicalTimingWheel` lives inside `HybridScheduler`, not `WorkerThread` | Timers are global; a single wheel with a dedicated ticker thread avoids per-worker duplication |
| `EDFPriorityQueue` is separate from `MultiPriorityWorkQueue` | The EDF queue indexes actors by deadline (global view); the per-worker deque holds runnable work items (local view). Separation avoids contention: the EDF queue is written under a brief spinlock, the Chase-Lev deque is lock-free |
| `MPSCMailbox<T>` is not templated on the Actor type | Actors receive `MessageVariant`; the mailbox is a pure data pipe with no Actor knowledge, keeping the dependency DAG clean |
| `MPSCMailbox::dequeue()` returns `nullptr` for partially-completed enqueues (ARM64 weak ordering) | Spinning inside `dequeue()` cannot make progress when the producer is asleep; the caller retries via the scheduler's `RequeueReady` mechanism after the producer's `mpsc_next` store becomes visible |
| `MultiLaneQueue` uses an 8-entry deferred-free ring buffer | OS-preempted producers may hold a dangling pointer across 1–2 `try_pop()` cycles; the ring buffer delays destruction by 8 cycles, bounding the UAF risk |
| `mailbox_was_empty_` pre-arm is held under the consumer spin-lock | Prevents a TOCTOU race where the producer sees the re-armed flag but the mailbox still has messages, suppressing the edge-triggered `notify_ready` |
| Post-`kIdle` double-check re-admits the actor if the mailbox became non-empty | Closes a lost-wakeup window on x86_64 TSO where the producer's enqueue completes before the consumer reaches `set(kIdle)`, causing the edge-trigger to see `kRunning` and drop the wakeup |
| `kLostWakeupRequeueBudget = 1` caps rate-limiter-driven re-admissions | Rate-limiter denial re-enqueues the message and returns `nullptr`, keeping the mailbox non-empty; without the cap the lost-wakeup check creates an unbounded spin loop |
| Platform-specific idle escalation constants (`kBackoffYieldIters`, `kBackoffSleepIters`) | Linux CFS `sched_yield` doesn't sleep (0 yields); macOS Mach `thread_switch` does (4 yields). Both use 4 nanosleep steps (180µs total) before CV |
| Backoff shift capped at 28 with 50ms ceiling via `std::min` | `10u << 31` overflows `uint32_t` to 0, producing `sleep_for(0us)` — indefinite 100% CPU spin on all workers |
| `backoff_counter_` is `std::atomic<uint32_t>` | The CLI thread reads it for `/scheduler workers` display; a plain read concurrent with the worker's write is a data race on ARM64 |
| EDF-aware CV timeout wakes 1ms before the earliest deadline | Ensures deadline-sensitive work is not delayed by a worker sleeping through it; the 100ms cap ensures shutdown responsiveness |
| Timer thread uses `next_deadline()` for deadline-based sleep | Replaces fixed 1ms polling (~1000 wakeups/sec) with on-demand sleep, reducing timer-thread CPU to near-zero under light load |
| `kRequeueBudget = 64` caps consecutive RequeueReady cycles | Prevents a high-traffic actor from monopolizing a worker; after the budget the actor goes through external `try_mark_ready`, naturally interleaving with other work |

---

### Core Interface Declarations

All declarations use C++20, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`). Return codes use `hpactor::error` where failure is recoverable, and asserts for programming errors.

#### `IScheduler`

```cpp
// include/hpactor/sched/scheduler.hpp
namespace hpactor::sched {

// Abstract scheduler interface.
// Owns the worker thread pool and routes actor activations to workers.
class IScheduler {
public:
    virtual ~IScheduler() = default;

    // Start all worker threads. Must be called before any enqueue().
    virtual void start() = 0;

    // Drain all queues and join all worker threads.
    virtual void stop() = 0;

    // Notify the scheduler that `actor` has at least one message in its mailbox.
    // Called by MPSCMailbox on the first enqueue into an empty mailbox.
    // Thread-safe; may be called from any thread including I/O threads.
    //
    // Internally, HybridScheduler::notify_ready calls select_worker(priority)
    // to pick a target worker, upserts the actor into EDFPriorityQueue with the
    // assigned worker index, then pushes a WorkItem onto that worker's deque.
    virtual void notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;

    // Hint that `actor` has exhausted its mailbox and should be marked idle.
    // Called by the worker loop after a drain finds the mailbox empty.
    virtual void notify_idle(ActorId actor) = 0;

    // Schedule a one-shot callback after `delay_ns` nanoseconds.
    // Returns a TimerHandle that can be passed to cancel_timer().
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;

    // Schedule a repeating callback every `interval_ns` nanoseconds.
    virtual TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) = 0;

    // Cancel a pending timer. Safe to call after the timer has already fired.
    virtual void cancel_timer(TimerHandle handle) = 0;

    // Returns the number of worker threads.
    virtual size_t worker_count() const = 0;

    using timer_callback = std::function<void()>;
};

} // namespace hpactor::sched
```

#### `HybridScheduler`

```cpp
// include/hpactor/sched/scheduler.hpp  (continued)
namespace hpactor::sched {

// Concrete scheduler: GEDF + multi-priority work stealing + A2WS.
class HybridScheduler final : public IScheduler {
public:
    struct Config {
        size_t   num_workers         = std::thread::hardware_concurrency();
        uint32_t priority_levels     = 4;         // P queue levels per worker
        uint32_t steal_threshold     = 2;         // τ: min imbalance to trigger steal
        uint32_t timer_resolution_us = 1000;      // base wheel tick = 1 ms
        uint32_t slab_slots          = 256;       // coroutine frame pool slots per thread
        size_t   slab_slot_size      = 512;       // bytes per slab slot
    };

    explicit HybridScheduler(ActorSystem& system, Config cfg = {});
    ~HybridScheduler() override;

    // IScheduler
    void        start()                                                    override;
    void        stop()                                                     override;
    void        notify_ready(ActorId, uint8_t priority, int64_t deadline) override;
    void        notify_idle(ActorId)                                       override;
    TimerHandle schedule_after(timer_callback, int64_t delay_ns)          override;
    TimerHandle schedule_every(timer_callback, int64_t interval_ns)       override;
    void        cancel_timer(TimerHandle)                                  override;
    size_t      worker_count() const                                       override;

    // Diagnostic: approximate ready-queue depth across all workers.
    size_t approx_queue_depth() const;

private:
    // Select the least-loaded worker index for a given priority level.
    // Algorithm:
    //   1. Read load snapshots for all workers via A2WSCoordinator.
    //   2. Among workers whose load[i] == min_load, choose by round-robin
    //      (using a per-priority atomic counter) to distribute evenly.
    //   3. If all workers are overloaded (load > HIGH_WATER_MARK), still route
    //      to the minimum-load worker — backpressure is handled at the caller.
    // Thread-safe: load snapshots are relaxed atomics; round-robin counter is
    // a per-priority fetch_add(1, relaxed) mod num_workers.
    size_t select_worker(uint8_t priority) const;

    ActorSystem&                        system_;
    Config                              cfg_;
    std::vector<WorkerThread>           workers_;
    EDFPriorityQueue                    edf_queue_;
    A2WSCoordinator                     a2ws_;
    HierarchicalTimingWheel             timer_wheel_;
    std::atomic<bool>                   running_{false};
};

} // namespace hpactor::sched
```

#### `WorkerThread`

```cpp
// include/hpactor/sched/worker_thread.hpp
namespace hpactor::sched {

// One OS thread with a local multi-priority deque and a coroutine frame pool.
class WorkerThread {
public:
    WorkerThread(size_t index, HybridScheduler& owner, const HybridScheduler::Config& cfg);
    ~WorkerThread();

    // Non-copyable, non-movable (owns a std::thread).
    WorkerThread(const WorkerThread&)            = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    void start();
    void stop();                   // sets stop flag, joins thread
    void push(WorkItem item);      // owner-side push (called by notify_ready routing)
    bool try_steal(WorkItem& out); // thief-side steal (called by A2WSCoordinator)

    // Load snapshot for A2WS (approximate queue depth, relaxed read).
    size_t load_snapshot() const;

    // Per-thread coroutine frame allocator. Accessible only from this thread.
    CoroutineFramePool& frame_pool();

    size_t index() const { return index_; }

    // ── Diagnostic accessors (safe for cross-thread reads) ──────────

    /// Total work items processed since start.
    uint64_t diag_work_found() const;
    /// Total idle loop iterations (polling, before CV escalation).
    uint64_t diag_idle_iters() const;
    /// Number of times the worker entered CV blocking.
    uint64_t diag_cv_escalations() const;
    /// CV wakeups triggered by enqueue-side notification.
    uint64_t diag_cv_notify_wakes() const;
    /// CV wakeups triggered by EDF-aware timeout expiry.
    uint64_t diag_cv_timeout_wakes() const;
    /// Whether the worker has escalated to CV-blocking idle model.
    bool diag_is_in_cv_model() const;
    /// Current backoff iteration count (atomic, safe from any thread).
    uint32_t diag_backoff_counter() const;

    /// Native OS thread identifier (kernel TID on Linux, pthread ID on macOS).
    uint64_t thread_id() const;

private:
    void thread_loop();
    void backoff();            // adaptive idle: yield → exponential sleep → capped at 50ms
    void reset_backoff();      // reset counter when work found

    size_t                    index_;
    HybridScheduler&          owner_;
    MultiPriorityWorkQueue    queues_;
    CoroutineFramePool        frame_pool_;
    std::thread               thread_;
    std::atomic<bool>         stop_{false};

    // Updated after each work item; read by A2WS neighbors via relaxed load.
    alignas(64) std::atomic<uint32_t> load_{0};   // cacheline-isolated

    // Adaptive idle escalation: increments on each idle iteration, resets to 0
    // when work is found.  Atomic for safe cross-thread reads from CLI/metrics
    // (plain read concurrent with worker's write is a data race on ARM64).
    std::atomic<uint32_t> backoff_counter_{0};

    // Kernel TID captured at thread start via syscall(SYS_gettid) on Linux,
    // pthread_threadid_np on macOS.  Written once, read from any thread.
    std::atomic<uint64_t> native_tid_{0};
};

} // namespace hpactor::sched
```

#### `ChaselevDeque<T>` and `MultiPriorityWorkQueue`

```cpp
// include/hpactor/sched/work_queue.hpp
namespace hpactor::sched {

// Chase-Lev work-stealing deque (dynamic circular array).
// push_bottom / pop_bottom: called only by the owning thread (wait-free).
// steal_top: called by any thief thread (lock-free, ABA-safe via version tag).
template<typename T>
class ChaselevDeque {
public:
    explicit ChaselevDeque(size_t initial_capacity = 256);

    // Owner operations — wait-free O(1) amortized (resizes on overflow).
    void push_bottom(T item);
    bool pop_bottom(T& out);

    // Thief operation — lock-free O(1).
    // Returns true and sets `out` on success; false if empty or lost CAS race.
    bool steal_top(T& out);

    size_t size_approx() const;   // relaxed read, approximate

private:
    struct CircularArray {
        std::vector<std::atomic<T>> buf;
        size_t                      mask;  // capacity - 1 (power-of-2 capacity)
        explicit CircularArray(size_t cap);
        T    get(int64_t i) const { return buf[i & mask].load(std::memory_order_relaxed); }
        void put(int64_t i, T v)  { buf[i & mask].store(v, std::memory_order_relaxed); }
        CircularArray* grow(int64_t bottom, int64_t top) const;
    };

    std::atomic<int64_t>        top_{0};
    std::atomic<int64_t>        bottom_{0};
    std::atomic<CircularArray*> array_;
    std::vector<CircularArray*> garbage_;   // old arrays pending reclamation
};

// WorkItem: what gets enqueued into a deque.
struct WorkItem {
    ActorId  actor;
    int64_t  deadline_ns;   // absolute CLOCK_MONOTONIC deadline; INT64_MAX if none
    uint64_t sequence;      // FIFO tiebreaker within same deadline
};

// Per-worker array of P Chase-Lev deques, one per priority level.
// Priority 0 = highest, P-1 = lowest (background).
class MultiPriorityWorkQueue {
public:
    explicit MultiPriorityWorkQueue(uint32_t priority_levels);

    // Push to a specific priority level. Owner thread only.
    void push(uint8_t priority, WorkItem item);

    // Pop from the highest non-empty level. Owner thread only.
    bool pop(WorkItem& out);

    // Steal from a specific priority level. Thief thread.
    bool steal(uint8_t priority, WorkItem& out);

    // Approximate total depth across all levels.
    size_t depth_approx() const;

    uint32_t num_levels() const { return static_cast<uint32_t>(levels_.size()); }

private:
    std::vector<ChaselevDeque<WorkItem>> levels_;
};

} // namespace hpactor::sched
```

#### `MPSCMailbox<T>`

```cpp
// include/hpactor/mailbox/mpsc_mailbox.hpp
namespace hpactor::mailbox {

// Intrusive Vyukov MPSC queue. T must provide:
//   std::atomic<T*> mpsc_next;   // intrusive link (zero extra allocation)
// Producers: wait-free enqueue.
// Consumer: lock-free dequeue (single consumer thread only).
template<typename T>
class MPSCMailbox {
public:
    // Callback invoked on the first enqueue into an empty mailbox (edge trigger).
    // Signature matches IScheduler::notify_ready to allow direct binding:
    //   mailbox.on_wake = [&sched](ActorId id, uint8_t p, int64_t d, void*) noexcept {
    //       sched.notify_ready(id, p, d);
    //   };
    // `priority` and `default_deadline_ns` are per-mailbox constants set at
    // Actor construction time and passed through on every edge-trigger call.
    using wake_fn = void (*)(ActorId, uint8_t priority, int64_t deadline_ns,
                             void* ctx) noexcept;

    explicit MPSCMailbox(ActorId owner,
                         uint8_t  priority         = 0,
                         int64_t  default_deadline = INT64_MAX,
                         wake_fn  on_wake          = nullptr,
                         void*    ctx              = nullptr);
    ~MPSCMailbox();

    // Producer: enqueue node. Wait-free. May be called from any thread.
    void enqueue(T* node) noexcept;

    // Consumer: dequeue next node; returns nullptr if empty.
    // MUST be called from a single consumer thread.
    //
    // IMPORTANT: Returns nullptr for THREE distinct conditions:
    //   1. Queue is genuinely empty (head_ == &stub_).
    //   2. ARM64 weak ordering: head_.exchange(acq_rel) is visible but the
    //      subsequent mpsc_next.store(release) is not yet visible.
    //   3. TOCTOU race during fixup: a producer enqueued between the head_
    //      check and the chain update, and mpsc_next is not yet visible.
    //
    // The caller MUST treat nullptr as "retry later" rather than a definitive
    // empty signal.  The scheduler's RequeueReady mechanism re-dispatches the
    // actor once the producer's store becomes visible (confirmed via count_).
    T* dequeue() noexcept;

    // Check whether the queue appears empty (based on atomic count_).
    // Uses acquire ordering — reflects all completed enqueues and dequeues.
    // Used by the lost-wakeup re-admission check: after set(kIdle), if
    // !mailbox->empty(), a message completed its enqueue during the transition
    // window and the actor must be re-admitted.
    bool empty() const noexcept;

    // Approximate element count (acquire load on count_).
    int64_t count() const noexcept;

    ActorId owner() const noexcept { return owner_; }

private:
    alignas(64) std::atomic<T*> head_;              // producers write here
    alignas(64) T*              tail_;              // consumer reads here (cacheline isolated)
    T                           stub_;              // sentinel node
    ActorId                     owner_;
    uint8_t                     priority_;
    int64_t                     default_deadline_ns_;
    wake_fn                     on_wake_;
    void*                       wake_ctx_;

    // Atomic element count.  Incremented AFTER mpsc_next store (enqueue),
    // decremented AFTER chain unlink (dequeue).  Synchronizes-with
    // count_.load(acquire) — used by empty() as a reliable "has work?"
    // oracle for lost-wakeup re-admission.
    std::atomic<int64_t>        count_{0};

    std::atomic<bool>           idle_{true};        // true when empty; CAS guards edge-trigger
};

} // namespace hpactor::mailbox
```

#### `EDFPriorityQueue`

```cpp
// include/hpactor/sched/edf_queue.hpp
namespace hpactor::sched {

// Global EDF view: tracks all actors that have been notified as ready,
// ordered by (priority_level ASC, deadline_ns ASC).
// Used by HybridScheduler to route notify_ready() to the right worker.
class EDFPriorityQueue {
public:
    struct EDFEntry {
        ActorId  actor;
        uint8_t  priority;
        int64_t  deadline_ns;
        size_t   target_worker;  // assigned worker index
    };

    // Insert or update an actor's scheduling entry.
    // Thread-safe (internally spinlocked — brief critical section).
    void upsert(EDFEntry entry);

    // Remove an actor (called when it becomes idle).
    void remove(ActorId actor);

    // Peek at the highest-priority/earliest-deadline pending entry.
    // Non-const: acquires the internal spinlock.
    // Returns false if queue is empty.
    bool peek_min(EDFEntry& out);

    // Pop the minimum entry. Returns false if queue is empty.
    bool pop_min(EDFEntry& out);

    size_t size() const;

private:
    mutable std::atomic_flag  lock_ = ATOMIC_FLAG_INIT;  // spinlock
    std::vector<EDFEntry>     heap_;  // binary min-heap; key: (priority, deadline_ns)

    static bool higher_priority(const EDFEntry& a, const EDFEntry& b);
};

} // namespace hpactor::sched
```

#### `A2WSCoordinator`

```cpp
// include/hpactor/sched/a2ws.hpp
namespace hpactor::sched {

// Manages the ring-topology load snapshot array used by A2WS.
// Each WorkerThread reads neighbors' snapshots to decide whether to steal.
class A2WSCoordinator {
public:
    explicit A2WSCoordinator(size_t num_workers, uint32_t steal_threshold);

    // Called by a worker after each work item to update its snapshot.
    // `load` is the worker's current approximate queue depth.
    void update_snapshot(size_t worker_index, uint32_t load) noexcept;

    // Query the best steal victim for `requester` at a specific priority level:
    //   1. Checks left and right ring neighbors' snapshots.
    //   2. Prefers the neighbor with the highest load at `priority` if imbalance > threshold.
    //   3. Falls back to checking all workers if both neighbors are empty at `priority`.
    //   4. Returns SIZE_MAX if no steal is warranted (imbalance ≤ threshold everywhere).
    //
    // `priority` is passed so the thief steals from the same level it needs work at,
    // respecting the PAS rule of targeting victims with earliest-deadline tasks first.
    size_t best_victim(size_t requester, uint8_t priority) const noexcept;

    // How many tasks to steal from a victim (halving heuristic).
    uint32_t steal_count(uint32_t own_load, uint32_t victim_load) const noexcept;

    // Returns true if the system is in "low-load" mode.
    // In low-load mode, stealing is restricted to num_workers/4 threads.
    bool is_low_load() const noexcept;

    // Number of workers permitted to steal concurrently.
    size_t max_concurrent_stealers() const noexcept;

private:
    size_t   num_workers_;
    uint32_t threshold_;

    // Each element occupies its own cache line to prevent false sharing.
    struct alignas(64) Snapshot {
        std::atomic<uint32_t> load{0};
    };
    std::vector<Snapshot>         snapshots_;
    std::atomic<uint32_t>         active_stealers_{0};
};

} // namespace hpactor::sched
```

#### `HierarchicalTimingWheel`

```cpp
// include/hpactor/timer/timing_wheel.hpp
namespace hpactor::timer {

using timer_callback = std::function<void()>;

struct TimerHandle {
    uint64_t id;    // 0 = invalid
    bool valid() const noexcept { return id != 0; }
};

// Intrusive timer entry. Callers embed this in their own structures.
struct TimerEntry {
    TimerHandle    handle;
    int64_t        expires_at_ns;  // absolute CLOCK_MONOTONIC
    int64_t        interval_ns;    // 0 = one-shot
    timer_callback cb;
    TimerEntry*    next = nullptr; // intrusive list link within a slot
};

// W-level hierarchical timing wheel.
// Single producer/consumer: all insert/cancel/advance calls must come from
// the same dedicated timer thread (or be externally serialized).
class HierarchicalTimingWheel {
public:
    struct Config {
        uint32_t levels       = 4;
        uint32_t base_slots   = 256;          // S₀; subsequent levels use 64 slots
        int64_t  base_tick_ns = 1'000'000;    // 1 ms base resolution
    };

    explicit HierarchicalTimingWheel(Config cfg = {});
    ~HierarchicalTimingWheel();

    // Insert a one-shot timer firing after `delay_ns` nanoseconds.
    TimerHandle insert_after(int64_t delay_ns, timer_callback cb);

    // Insert a repeating timer firing every `interval_ns` nanoseconds.
    TimerHandle insert_every(int64_t interval_ns, timer_callback cb);

    // Cancel a pending timer. Safe to call after the timer has fired.
    void cancel(TimerHandle handle);

    // Advance the wheel to `now_ns` (CLOCK_MONOTONIC), firing all expired timers.
    // Calls each expired callback inline on the calling (timer) thread.
    // Returns the number of timers that fired.
    //
    // Thread safety: advance() is NOT thread-safe with insert_after/insert_every/cancel.
    // All four methods must be called from the single dedicated timer thread.
    //
    // Timer callbacks (e.g., scheduler.notify_ready) are themselves thread-safe —
    // IScheduler::notify_ready is documented as callable from any thread.
    // Callbacks must not re-enter insert_after/cancel (no recursive wheel mutation).
    int advance(int64_t now_ns);

    // Time of the next scheduled timer expiry, or INT64_MAX if no timers pending.
    int64_t next_expiry_ns() const;

private:
    struct Slot {
        TimerEntry* head = nullptr;  // intrusive singly-linked list
    };

    struct Wheel {
        std::vector<Slot> slots;
        uint32_t          current = 0;   // cur[k]
        int64_t           resolution_ns;
    };

    void place(TimerEntry* entry);            // insert into correct level + slot
    void cascade(size_t level, uint32_t slot); // move entries from level k → k-1

    std::vector<Wheel>                         wheels_;
    std::unordered_map<uint64_t, TimerEntry*>  entries_;  // handle.id → entry
    uint64_t                                   next_id_{1};
    int64_t                                    base_tick_ns_;
    int64_t                                    last_advance_ns_{0};
};

} // namespace hpactor::timer
```

#### `CoroutineFramePool`

```cpp
// include/hpactor/memory/coroutine_frame_pool.hpp
namespace hpactor::memory {

// Thread-local slab pool for coroutine frames.
// Intended as a data member of WorkerThread; must only be accessed
// from the owning worker thread.
class CoroutineFramePool {
public:
    static constexpr size_t DEFAULT_SLOT_SIZE  = 512;
    static constexpr size_t DEFAULT_SLOT_COUNT = 256;

    explicit CoroutineFramePool(
        size_t slot_size  = DEFAULT_SLOT_SIZE,
        size_t slot_count = DEFAULT_SLOT_COUNT);
    ~CoroutineFramePool();

    // Allocate a slot of at least `size` bytes.
    // O(1): bitmap TZCNT + conditional store.
    // Falls back to the global heap only when size > slot_size_ or pool exhausted.
    [[nodiscard]] void* allocate(size_t size) noexcept;

    // Release a slot returned by allocate().
    void deallocate(void* ptr) noexcept;

    // Diagnostic counters.
    size_t hits()     const noexcept { return hits_; }
    size_t misses()   const noexcept { return misses_; }  // fell back to global heap
    size_t capacity() const noexcept { return slot_count_; }

private:
    size_t                      slot_size_;
    size_t                      slot_count_;
    std::unique_ptr<uint8_t[]>  storage_;    // slot_count_ * slot_size_ bytes
    std::vector<uint64_t>       bitmap_;     // 1 bit per slot; 1 = free
    size_t                      hits_{0};
    size_t                      misses_{0};
};

} // namespace hpactor::memory
```

---

### MPSCMailbox Concurrency and Correctness

The Vyukov MPSC queue is the central data structure for all actor message delivery. Production use under concurrent multi-producer load revealed three architectural correctness concerns that are now baked into the scheduler's contract with the mailbox.

#### ARM64 Visibility Gap

On weakly-ordered architectures (ARM64), `head_.exchange(acq_rel)` can become visible to the consumer **before** the producer's subsequent `mpsc_next.store(release)`. The consumer observes a partially-completed enqueue: `head_` points past the stub (the queue appears non-empty), but the next node's `mpsc_next` is still `nullptr`.

**Design decision: return `nullptr` immediately, do not spin.** Spinning cannot make progress when the producer thread is asleep or preempted. Instead, `dequeue()` returns `nullptr` for any partially-completed enqueue. The caller — `MPSCActorMailbox::try_pop()` and ultimately `BehaviorActorRunner::run()` — treats `nullptr` as "retry later" rather than a definitive empty signal:

1. The actor is set to `kIdle`.
2. A post-`kIdle` double-check of `!mailbox->empty()` (using `count_`, which is incremented **after** the `mpsc_next` store) detects the now-visible message.
3. The actor is re-admitted via `try_mark_ready()` — the lost-wakeup re-admission path (see Wakeup Protocol section).

This design eliminates the need for a spin-loop inside `dequeue()` on architectures where producer visibility is not guaranteed within bounded time. The queue's `count_` field serves double duty: admission/pressure monitoring under normal operation, and as a reliable "is there work hiding behind a partially-visible enqueue?" oracle during scheduler re-admission.

#### TOCTOU Fixup Spin-Wait

When dequeuing the **last element A** from the queue, a time-of-check-to-time-of-use (TOCTOU) race exists between the consumer checking `head_` and updating the chain:

```
Consumer                              Producer
─────────                             ────────
next_next = A->mpsc_next = nullptr
h = head_ = A
h == next → skip first spin-wait
                                      prev = head_.exchange(B) → head_ = B
                                      ── OS-PREEMPTED ── (before mpsc_next store)
t->mpsc_next = nullptr    // chain broken
h = head_ = B ≠ A  → enter fixup
// OLD: next_next = A->mpsc_next → nullptr → silently skip fixup
// BUG: stub_.mpsc_next stays nullptr PERMANENTLY
```

With `head_ = B` (valid node) and `stub_.mpsc_next = nullptr`, the queue is unreachable: no future enqueue can set `stub_.mpsc_next` because subsequent producers capture `prev = head_` which is B (or later nodes), never `&stub_`. The next `dequeue()` spins forever.

**Fix:** The fixup block now **spin-waits** on `next->mpsc_next` until the producer's store becomes visible, matching the existing spin-wait pattern earlier in `dequeue()`:

```cpp
do {
    next_next = next->mpsc_next.load(std::memory_order_acquire);
} while (next_next == nullptr);
t->mpsc_next.store(next_next, std::memory_order_release);
```

This spin is bounded by the producer's remaining timeslice (the producer is executing the `mpsc_next` store on a running core) — unlike the ARM64 visibility gap where the producer may be asleep, here the producer is guaranteed to complete within microseconds.

#### Deferred-Free Ring Buffer

`MultiLaneQueue` (the composite queue layer that owns per-lane `MPSCMailbox` instances) uses an **8-entry deferred-free ring buffer** (`kPendingFreeRingSize = 8`) for node destruction.

**Problem:** `enqueue()` is a two-step protocol:
1. `prev = head_.exchange(node)` — captures a pointer to the old head
2. `prev->mpsc_next.store(node)` — writes to the captured node

A producer preempted between steps 1 and 2 holds a dangling pointer if the consumer frees `prev` before step 2 completes. The `store` lands on freed memory, corrupting the heap and (critically) failing to update the MPSC chain — `stub_.mpsc_next` stays `nullptr` permanently while `head_` points past the stub, causing the consumer to spin forever.

**Solution:** Deferred destruction. Nodes dequeued by `try_pop()` are staged in the ring buffer via `set_pending_free(node)`. The node is destroyed only after 8 subsequent `set_pending_free()` calls (or at mailbox destruction via `drain_pending_free()`). This gives OS-preempted producers up to 8 logical dequeue cycles to complete their `mpsc_next` store to valid memory.

The ring buffer replaces the earlier single-slot design which was insufficient: a producer preempted across 2+ `try_pop()` cycles could still hit freed memory.

---

### Wakeup Protocol and Lost-Wakeup Prevention

The scheduler's edge-triggered wakeup protocol coordinates between the mailbox producer path (any thread) and the consumer path (scheduler worker). Four distinct mechanisms prevent messages from being orphaned in a non-empty mailbox with no pending `WorkItem`.

#### Edge-Triggered Wakeup

The fundamental contract: `notify_ready()` is called **exactly once** per non-empty transition. `MPSCActorMailbox` uses a CAS on `mailbox_was_empty_`:

```
Producer (any thread):
  1. enqueue message into MultiLaneQueue
  2. if CAS(mailbox_was_empty_, true → false) succeeds:
       scheduler.notify_ready(actor_id, priority, deadline)
```

If the CAS fails, the mailbox was already non-empty — the actor is already scheduled or executing, and will drain the new message naturally. No redundant `notify_ready` call is made.

#### Pre-Arm Race Fix

The `mailbox_was_empty_` flag must be re-armed (set to `true`) when the mailbox drains to empty, so the next enqueue triggers a fresh `notify_ready`. The original code re-armed the flag **before** acquiring the consumer spin-lock, creating a race window:

```
Consumer                              Producer
─────────                             ────────
mailbox_was_empty_ = true  (pre-arm)
                                      empty() → false (messages still present)
                                      ── skips CAS (was_empty = true already) ──
lock_consumer()
dequeue last message → mailbox empty
                                      // No wakeup — message orphaned
```

**Fix:** The pre-arm now happens **under** the consumer lock:

```
Consumer:
  lock_consumer()
  mailbox_was_empty_ = true   // pre-arm held under lock
  dequeue until empty
  unlock_consumer()
```

This serializes the producer's `empty()` check with the consumer's pre-arm:
- Producer checks before lock → sees non-empty → no wakeup needed (consumer is about to dequeue)
- Producer checks after unlock → sees the re-armed flag → CAS fires correctly → `notify_ready`

#### Lost-Wakeup Re-Admission

When `try_pop()` returns `nullptr` (ARM64 visibility gap, genuine empty, or rate-limiter denial), `BehaviorActorRunner::run()` sets the actor to `kIdle`. On x86_64 TSO, the producer's full enqueue (including `mpsc_next` store) can complete **before** the consumer reaches `set(kIdle)` — the edge-triggered `notify_ready` then sees `kRunning` and returns `AlreadyRunning`, dropping the wakeup.

**Fix:** A post-`kIdle` double-check:

```cpp
actor_state.set(ActorState::kIdle);
if (!mailbox->empty()) {
    auto admission = ready_gate_.try_mark_ready(actor_id);
    if (admission.accepted() || admission.code == AlreadyReady) {
        return RequeueReady;  // re-admit to the scheduler
    }
}
```

`mailbox->empty()` reads `count_` with `acquire` ordering, which synchronizes with the producer's `count_.fetch_add(1, release)` — guaranteeing `mpsc_next` visibility when `count_ > 0`. The re-admission is architecture-independent and safe on all platforms.

#### Rate-Limiter Spin-Loop Guard

`MPSCActorMailbox::dequeue()` has a rate-limiter path: when an actor's token bucket is empty, the message is **re-enqueued to lane 0** and `dequeue()` returns `nullptr` — even though the mailbox is still non-empty. Without a guard, the lost-wakeup double-check sees `!empty()` and re-admits the actor, creating an unbounded spin loop:

```
try_pop → dequeue → rate-limiter denies → re-enqueue → return nullptr
       → set kIdle → mailbox non-empty → re-admit via try_mark_ready
       → dispatch → try_pop → … (loop until token refill)
```

**Fix:** A sequence-based budget guard on the lost-wakeup re-admission path:

```cpp
static constexpr uint64_t kLostWakeupRequeueBudget = 1;
actor_state.set(ActorState::kIdle);
if (!mailbox->empty() && item.sequence < kLostWakeupRequeueBudget) {
    auto admission = ready_gate_.try_mark_ready(item.actor);
    if (admission.accepted() || admission.code == AlreadyReady) {
        return {ActorRunDisposition::RequeueReady, 0, INT64_MAX};
    }
}
```

A single re-dispatch suffices for the genuine lost-wakeup window (the `mpsc_next` store becomes visible before the next `try_pop`). Rate-limited actors whose tokens are exhausted are capped to one re-admission, then suspend until the next external wakeup (timer, cross-actor message, token refill).

---

### Worker Adaptive Idle and CPU Reduction

The scheduler reduces idle CPU consumption through two mechanisms: adaptive worker idle escalation and deadline-based timer thread sleep.

#### Idle Escalation Model

Each worker thread escalates through three idle stages when no work is found in local queues or via stealing:

```
┌──────────┐    ┌──────────────────────┐    ┌───────────────────┐
│  yield   │───▶│ exponential backoff   │───▶│ CV blocking        │
│ (0 or 4) │    │ 10→20→50→100 µs       │    │ EDF-aware timeout  │
└──────────┘    └──────────────────────┘    └───────────────────┘
    stage 1            stage 2                   stage 3
```

**Stage 1 — Yield:** Short spin phase. On macOS (`kBackoffYieldIters = 4`), `sched_yield` uses Mach `thread_switch` which actually relinquishes the CPU. On Linux (`kBackoffYieldIters = 0`), `sched_yield` merely rotates the CFS run-queue without sleeping — yielding is skipped entirely in favor of direct nanosleep.

**Stage 2 — Exponential backoff:** Four nanosleep iterations at fixed durations: 10µs, 20µs, 50µs, 100µs (total 180µs). These are architecture-independent and use only standard `std::this_thread::sleep_for`. The short windows catch work from cascading message sends without the overhead of CV syscalls.

**Stage 3 — CV blocking:** After `kPollThreshold` consecutive idle iterations (4 on Linux, 8 on macOS), the worker enters condition-variable blocking. Before blocking, the worker sets `is_blocking_ = true` on its placement-layer `WorkerState` so that enqueue-side code can signal `sleep_cv_` for immediate wakeup.

**Backoff counter reset:** `backoff_counter_` resets to 0 whenever work is found (local pop, steal, or CV pre-block re-check). This keeps workers responsive: an idle period of any length is followed by immediate low-latency polling when work resumes.

**Backoff overflow prevention:** The exponential backoff `10µs * 2^shift` caps the shift at 28 (max `10µs * 2^28 = 2.68s`, then clamped to 50ms by `std::min`). An uncapped shift > 28 overflows `uint32_t`: `10u << 31 = 0` — producing `sleep_for(0us)` which spins the core at 100% indefinitely. This bug affected all workers on multi-core systems after ~40 consecutive idle iterations (~10–30s wall time).

#### Platform-Specific Constants

| Constant | Linux | macOS | Rationale |
|:---------|:-----:|:-----:|:----------|
| `kBackoffYieldIters` | 0 | 4 | Linux CFS doesn't sleep on `sched_yield`; Mach does |
| `kBackoffSleepIters` | 4 | 4 | 180µs total nanosleep before CV |
| `kPollThreshold` | 4 | 8 | Sum of yield + sleep iterations |

These are `static constexpr` at file scope in `src/sched/worker_thread.cpp`, guarded by `#if defined(__linux__)` / `#elif defined(__APPLE__)`.

#### EDF-Aware CV Timeout

When a worker enters CV blocking, it computes a timeout from the scheduler's EDF queue:

```cpp
int64_t edf_ns = owner_->edf_next_deadline();
if (edf_ns != INT64_MAX) {
    auto delta_ms = (edf_ns - now_ns) / 1'000'000;
    auto margin = 1ms;  // wake 1ms before deadline for steal + dispatch headroom
    timeout = clamp(delta_ms - margin, 1ms, 100ms);
}
```

The worker sleeps until 1ms before the earliest deadline, then wakes to process or steal the deadline-sensitive work. The 100ms cap ensures shutdown responsiveness — `stop()` signals `sleep_cv_` and the worker wakes within 100ms even if no deadline is pending.

#### Timer Thread Deadline-Based Sleep

The dedicated timer thread uses `next_deadline()` on the active timer backend (`TimingWheel` or `CalendarQueue`) to sleep until the next timer expiry:

```cpp
int64_t next_ns = timer_wheel_.next_deadline();
auto sleep_duration = clamp(next_ns - now_ns, 0ns, 100ms);
std::this_thread::sleep_for(sleep_duration);
```

This replaces the original fixed 1ms polling loop, reducing timer-thread wakeups from ~1000/sec to on-demand (typically 1–10/sec under load). The 100ms cap provides the same shutdown responsiveness as the worker CV timeout.

#### Diagnostic Exposure

The worker's idle state is exposed for CLI introspection via `WorkerSnapshot`:

| Accessor | Description |
|:---------|:------------|
| `diag_is_in_cv_model()` | `true` when `backoff_counter_ >= kPollThreshold` |
| `diag_backoff_counter()` | Current backoff iteration count (`std::atomic<uint32_t>`) |
| `diag_idle_iters()` | Total idle loop iterations (polling, before CV) |
| `diag_cv_escalations()` | Number of times worker entered CV blocking |
| `diag_cv_notify_wakes()` | CV wakeups triggered by enqueue-side notification |
| `diag_cv_timeout_wakes()` | CV wakeups triggered by EDF-aware timeout expiry |

`backoff_counter_` is `std::atomic<uint32_t>` (was plain `uint32_t`) — the CLI thread reads it for `/scheduler workers` display, and a plain read concurrent with the worker's write is a data race on ARM64.

---

### Fairness and Actor Dispatch Budgeting

The execution engine enforces two budget caps to prevent actor starvation and CPU monopolization.

#### RequeueReady Path

When `BehaviorActorRunner::run()` processes one message and the mailbox is still non-empty, the actor is re-enqueued via the fast path:

```cpp
auto admission = ready_gate_.mark_ready_already_admitted(actor);
if (admission.accepted() || admission.code == AlreadyReady) {
    return {ActorRunDisposition::RequeueReady, 0, INT64_MAX};
}
```

`mark_ready_already_admitted()` transitions the actor from `kRunning` → `kReady` without going through the external `notify_ready` → `try_mark_ready` path. This is faster (no EDF upsert, no worker selection) and preserves the actor's existing worker affinity.

#### Requeue Budget Cap

`kRequeueBudget = 64` caps consecutive `RequeueReady` cycles:

```cpp
static constexpr uint64_t kRequeueBudget = 64;
bool budget_exhausted =
    !context.workers_paused && item.sequence >= kRequeueBudget;

if (!mailbox->empty() && !budget_exhausted) {
    // fast path: RequeueReady
}
// Budget exhausted → force kIdle, re-admit via try_mark_ready (slow path)
```

The sequence counter in `WorkItem` is incremented by the execution engine on each `RequeueReady` round-trip. After 64 consecutive cycles, the actor is forced to `kIdle` and re-admitted via `try_mark_ready` — the external notification path. This naturally interleaves the actor with other work because `try_mark_ready` goes through worker selection and EDF upsert, allowing other actors to be picked first.

The budget is skipped when workers are paused (`context.workers_paused` = deterministic test mode) to avoid perturbing test interleavings.

---

### Actor Scheduling Lifecycle State Machine

Each actor's scheduling state is encoded in the `flags` field of `state_word`. The state machine governs when the scheduler creates, runs, and destroys a `WorkItem` for an actor.

```
                          ┌───────────────────────────────────────────┐
                          │                                           │
        spawn()           ▼                                           │
  ──────────────────▶ [ IDLE ]◀────────────────────────────┐         │
                          │                                 │         │
     first message        │ MPSCActorMailbox::enqueue()     │         │
     arrives              │ edge-trigger CAS fires          │         │
     (CAS was_empty)      │ → notify_ready()                │         │
                          ▼                                 │         │
                     [ SCHEDULED ]                          │         │
                   (WorkItem in deque,                      │         │
                    EDF entry upserted)                     │         │
                          │                                 │         │
    worker picks          │ execute_actor()                 │         │
    it up via             │ → actor_state:                  │         │
    try_execute_one()     │   SCHEDULED→EXECUTING           │         │
                          ▼                                 │         │
                     [ EXECUTING ]                          │         │
                          │                                 │         │
              ┌───────────┼───────────────┐                 │         │
              │           │               │                 │         │
     try_pop  │  mailbox │   mailbox     │                 │         │
     →nullptr │  drained  │   non-empty  │                 │         │
              │           │               │                 │         │
              ▼           ▼               ▼                 │         │
        [ IDLE* ]   [ IDLE ]      [ RequeueReady ]─────────┘         │
         double-                        │                            │
         check:                    (fast path:                       │
    ┌─ !empty()?                 mark_ready_                         │
    │   YES → try_mark_ready     already_admitted,                   │
    │   → RequeueReady           capped at 64                        │
    │                            cycles per                         │
    │   NO → stay IDLE           kRequeueBudget)                    │
    │                                                                │
    └── * lost-wakeup re-admission path                              │
        (ARM64 nullptr, x86_64 TSO window,                           │
         kLostWakeupRequeueBudget = 1 guard)                         │
                          │
              co_await    │ actor calls async I/O
              or I/O op   │ (e.g., async_recv)
                          ▼
                    [ IO_WAITING ]
                          │
              I/O         │ EventLoop delivers
              completes   │ OpCompletion → notify_ready()
                          ▼
                     [ SCHEDULED ]  ◀── re-enters the deque
                          │
        error /           │ actor returns
        terminate()       │ ActorExitReason
                          ▼
                    [ TERMINATED ]
                     (ref count → 0,
                      resources freed)
```

**State flag encoding** in `state_word[23:16]`:

| Flag bit | Name | Set when | Cleared when |
| :------- | :--- | :------- | :----------- |
| bit 23 | `executing` | Worker begins processing an actor | Worker finishes or suspends |
| bit 22 | `scheduled` | Actor enqueued in a deque (via `try_mark_ready`) | Worker begins processing |
| bit 21 | `io_waiting` | Actor awaits I/O completion | I/O completion delivered |
| bit 20 | `suspended` | Actor voluntarily yields | Re-scheduled by scheduler |
| bit 19 | `terminated` | Actor has exited | Never (terminal state) |
| bit 18 | `monitored` | Another actor monitors this one | Monitor actor terminates |
| bit 17 | `linked` | Linked to another actor | Link broken or partner terminates |
| bit 16 | `rescheduled` | New message arrived while executing | Next scheduling poll |

**Scheduler-level states** (not bit flags — derived from `ActorState` machine word):

| State | Meaning | Typical transition |
|:------|:--------|:-------------------|
| `kIdle` | No messages, no pending work item | Set after mailbox drain or `try_pop` returns `nullptr` |
| `kReady` | Messages pending, work item enqueued | Set by `try_mark_ready()` (external) or `mark_ready_already_admitted()` (fast path) |
| `kRunning` | Worker is currently executing this actor | Set by `execute_actor()` CAS `kReady→kRunning` |

**Mutual exclusion invariants:**
- `executing` and `scheduled` are mutually exclusive: a WorkItem is never in a deque while the actor is being run.
- `executing` and `io_waiting` are mutually exclusive: transitioning to `IO_WAITING` clears `executing` atomically via a single `compare_exchange_strong` on `state_word`:
  ```
  // EXECUTING → IO_WAITING transition (single CAS):
  uint32_t expected = old_word | FLAG_EXECUTING;
  uint32_t desired  = (old_word & ~FLAG_EXECUTING) | FLAG_IO_WAITING;
  state_word.compare_exchange_strong(expected, desired, acq_rel);
  ```
- `terminated` is absorbing: once set, no other flag may be set.
- `rescheduled` may be set concurrently with `executing` (by a producer on another thread); the consumer checks it after completing the current message before deciding whether to re-enqueue (`RequeueReady`) or go `kIdle`.
- **`kIdle` → `kReady` re-admission:** After setting `kIdle`, the execution engine double-checks `!mailbox->empty()`. If the mailbox became non-empty during the transition window, the actor is re-admitted via `try_mark_ready()` — this is the lost-wakeup re-admission path, guarded by `kLostWakeupRequeueBudget = 1` to prevent rate-limiter spin loops.

---

### Message Delivery Flow

This section traces the complete path of a single message from producer to consumer.

#### Step-by-step: `context()->send(target, msg)`

```
Thread A (producer)                Worker Thread B (consumer)
─────────────────────────          ──────────────────────────────────

1. ActorContext::send(target, msg)
   │
   ├─ Allocate Message node from
   │  CoroutineFramePool (or heap)
   │
   ├─ msg_node.mpsc_next = nullptr
   │  (relaxed store)
   │
   ├─ MPSCMailbox::enqueue(msg_node)
   │   ├─ prev = head_.exchange(msg_node, acq_rel)   ← LP₁
   │   └─ prev->mpsc_next.store(msg_node, release)   ← LP₂
   │
   └─ if (mailbox.idle_.exchange(false, acq_rel))
       └─ scheduler.notify_ready(target, priority, deadline)
              │
              ├─ select_worker(priority) → worker_idx
              ├─ EDFQueue::upsert({target, priority, deadline, worker_idx})
              └─ workers_[worker_idx].push(WorkItem{target, deadline, seq++})

                                   2. WorkerThread run_loop:
                                   try_execute_one():
                                     ├─ queues_.pop(item)        ← gets WorkItem
                                     ├─ state_word: kReady→kRunning (CAS)
                                     │
                                     ├─ try_pop(msg) from mailbox
                                     │   ├─ Success (msg popped):
                                     │   │   ├─ is_expired? → DLQ + metric
                                     │   │   └─ valid → actor.receive(msg)
                                     │   │       └─ Behavior::invoke(msg.payload)
                                     │   └─ Failure (nullptr):
                                     │       ├─ ARM64: partial enqueue visible
                                     │       ├─ Genuine empty: mailbox drained
                                     │       └─ Rate-limiter denial: msg re-enqueued
                                     │
                                     ├─ try_pop returned msg, mailbox non-empty:
                                     │   ├─ RequeueReady (mark_ready_already_admitted)
                                     │   └─ capped at kRequeueBudget = 64 cycles
                                     │
                                     ├─ try_pop returned nullptr:
                                     │   ├─ state_word: kRunning→kIdle
                                     │   ├─ double-check: !mailbox->empty()?
                                     │   │   └─ YES + seq < kLostWakeupRequeueBudget:
                                     │   │       try_mark_ready → RequeueReady
                                     │   └─ NO → SuspendedOrIdle
                                     │
                                     └─ mailbox empty after msg:
                                         ├─ state_word: kRunning→kIdle
                                         ├─ !mailbox->empty()? → try_mark_ready
                                         └─ scheduler.notify_idle(target)
                                             └─ EDFQueue::remove(target)

                                   3. A2WSCoordinator:
                                   update_snapshot(B_index, queues_.depth_approx())
                                     └─ snapshots_[B].load.store(depth, relaxed)

                                   4. Neighboring idle worker C:
                                   best_victim(C_index, priority) → returns B_index
                                     └─ queues_[B].steal(priority, item)
                                         └─ ChaselevDeque::steal_top(item)
                                             └─ CAS on top_ with version tag ← ABA-safe
```

#### I/O-Waiting Path

When an actor calls `co_await async_recv(fd, ...)`:

```
Actor coroutine (on Worker B)        EventLoop (I/O thread)
─────────────────────────────        ──────────────────────────

co_await async_recv(fd, bufs, actor_id)
│
├─ EventLoop::add_fd(fd, Read)
│   └─ EpollBackend::add_fd(fd, EPOLLIN|EPOLLET)
│
├─ state_word: EXECUTING → IO_WAITING  (single CAS)
│
└─ coroutine suspends (returns to
   WorkerThread run_loop; B is free
   to process other actors)

                                     epoll_wait fires (ET mode):
                                     EpollBackend::wait() → events
                                       └─ EventLoop::enqueue_completion(
                                              OpCompletion{actor_id, Recv, fd, n})
                                            └─ ActorSystem::enqueue_completion()
                                                └─ scheduler.notify_ready(
                                                       actor_id, priority, deadline)
                                                     └─ WorkerThread::push(WorkItem)

Actor resumes on next available worker:
coroutine_handle.resume()
  └─ co_await result = OpCompletion{...}
```

**Key invariant:** Between `IO_WAITING` and the next `notify_ready`, the actor's `WorkItem` is not in any worker deque — it is solely represented by the pending I/O subscription. This prevents double-execution and eliminates the need for a lock during the `IO_WAITING → SCHEDULED` transition.

---

### Interface Interaction Summary

```
                        ┌──────────────────────────────────────┐
                        │            ActorSystem               │
                        │  spawn() · send() · enqueue_compl.  │
                        └──────────────┬───────────────────────┘
                                       │ notify_ready / notify_idle
                        ┌──────────────▼───────────────────────┐
                        │          HybridScheduler             │
                        │  start · stop · schedule_after/every │
                        └──┬──────────────┬────────────────────┘
                           │              │              │
              ┌────────────▼──┐  ┌────────▼──────┐  ┌───▼──────────────────┐
              │  WorkerThread │  │ EDFPriority   │  │ HierarchicalTiming   │
              │  [0 .. m-1]   │  │ Queue         │  │ Wheel                │
              │               │  │ (global EDF   │  │ (dedicated timer     │
              │ ┌─────────────┤  │  index)       │  │  thread)             │
              │ │MultiPriority│  └───────┬────────┘  └──────────────────────┘
              │ │ WorkQueue   │          │ upsert / remove
              │ │ (P deques)  │          │
              │ └─────────────┤  ┌───────┴─────────────┐
              │               │  │  A2WSCoordinator    │
              │ CoroutineFrame│  │  (ring snapshots)   │
              │ Pool          │  └─────────────────────┘
              └───────────────┘          ▲
                      │                  │ update_snapshot / best_victim(idx, priority)
                      └──────────────────┘
                  (Worker updates A2WS after each work item)

              ┌───────────────────────────────────────┐
              │          MPSCMailbox<T>               │
              │  (one per actor; owned by ActorSystem)│
              │  enqueue (producers) · dequeue (sched)│
              └───────────────────────────────────────┘
```
