# Architecture Design: Coroutine Scheduling Subsystem

## Executive Summary

This document refines the high-level M:N coroutine scheduling design from the [Scheduling Architecture Design](../architecture/scheduling/scheduling-architecture-design.md) with concrete implementation details for the HPActor framework. It covers the `CoroutineTask` abstraction, actor state machine integration, awaiter patterns, and integration with the existing `HybridScheduler`, `TimingWheel`, and `CoroutineFramePool`.

---

## 1. Design Goals

1. **M:N Mapping**: Map M user-space actors (C++20 stackless coroutines) onto N worker threads with minimal overhead.
2. **Two-Level Scheduling**: Decouple message delivery (Level 1) from CPU execution (Level 2).
3. **Cache Locality**: Prefer keeping an actor on the same worker across messages.
4. **Work Stealing**: Use `HybridScheduler`'s A2WS to balance load across workers.
5. **No Exceptions/RTTI**: Per project constraints, use `error` codes and `std::variant` instead.

---

## 2. Core Abstractions

### 2.1 CoroutineTask

`CoroutineTask` is the return type of actor coroutines, wrapping `std::coroutine_handle<CoroutinePromise>` and managing actor lifecycle.

```cpp
// include/hpactor/sched/coroutine_task.hpp
namespace hpactor::sched {

// Forward declare
class ActorCoroutine;

// Coroutine promise type - controls actor lifecycle
struct CoroutinePromise {
    using handle_type = std::coroutine_handle<CoroutinePromise>;

    ActorId actor_id;
    ActorState state{ActorState::Idle};  // atomic state
    WorkerThread* owner{nullptr};        // which worker owns this actor

    // Mailbox integration
    MPSCMailbox<MessageNode>* mailbox{nullptr};
    std::atomic<bool> mailbox_was_empty{true};

    // Suspension
    std::coroutine_handle<> continuation;  // for chained awaiters

    CoroutinePromise() = default;
    ~CoroutinePromise();

    // Called when coroutine first starts
    void initial_suspend() noexcept {
        // Start in suspended state - scheduler decides when to resume
        std::suspend_always{};
    }

    // Called when coroutine reaches co_return or throws
    void return_void() noexcept {
        set_state(ActorState::Terminated);
    }

    void unhandled_exception() noexcept {
        set_state(ActorState::Terminated);
        // Store exception info for error reporting
    }

    // Create the coroutine handle and return CoroutineTask
    CoroutineTask get_return_object();

    // Actor state transitions
    enum class ActorState : uint8_t {
        Idle       = 0,  // Suspended, mailbox empty
        Ready      = 1,  // In scheduler queue, has pending messages
        Running    = 2,  // Currently executing on a worker
        IOWaiting  = 3,  // Waiting for I/O completion
        Terminated = 4   // Coroutine has finished
    };

    void set_state(ActorState new_state);
    ActorState get_state() const;

    // Called by MailboxAwaiter when mailbox becomes non-empty
    void notify_mailbox_nonempty();
};

} // namespace hpactor::sched
```

### 2.2 ActorState Machine

```
                         spawn()
                           │
                           ▼
                      ┌─────────┐
        ┌─────────────│  Idle   │◀────────────────┐
        │             └────┬────┘                 │
        │                  │ first message        │ mailbox empty
        │                  │ (edge-trigger)       │ after drain
        │                  ▼                     │
        │             ┌─────────┐                │
        │             │  Ready  │────────────────┤
        │             │(queued) │                │
        │             └────┬────┘                │
        │                  │ worker picks up     │
        │                  ▼                     │
        │             ┌─────────┐                │
        │             │ Running │────────────────┘
        │             └────┬────┘
        │                  │
        │   co_await       │ I/O operation
        │   or timeout     ▼
        │             ┌──────────┐
        └─────────────│ IOWaiting│───(I/O completes)──▶ Ready
                      └──────────┘
        │
        │ co_return or error
        ▼
  ┌─────────────┐
  │ Terminated  │
  └─────────────┘
```

### 2.3 MailboxAwaiter

Custom awaitable for `co_await mailbox.receive()`:

```cpp
// include/hpactor/sched/mailbox_awaiter.hpp
namespace hpactor::sched {

class MailboxAwaiter {
public:
    explicit MailboxAwaiter(ActorCoroutine& actor) noexcept;

    // Can suspend if mailbox is empty
    bool await_ready() const noexcept {
        return !actor_.mailbox_is_empty();
    }

    // Called when suspending - set state to Idle
    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        // Atomic transition: Running → Idle
        ActorState expected = ActorState::Running;
        if (actor_.compare_exchange_state(expected, ActorState::Idle)) {
            // Successfully suspended - store continuation for later
            actor_.continuation_ = continuation;

            // If a message arrived between await_ready() and here,
            // we need to re-schedule. The sender's CAS on state will handle this.
        }
    }

    // Called when resuming
    void await_resume() noexcept {
        // State should already be Ready or Running
    }

private:
    ActorCoroutine& actor_;
};

// Specialization for blocking receive (stackful coroutine)
class BlockingMailboxAwaiter {
public:
    explicit BlockingMailboxAwaiter(ActorCoroutine& actor,
                                    CoroutineFramePool::Frame* frame) noexcept;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> continuation) noexcept;

    MessageVariant await_resume() noexcept;

private:
    ActorCoroutine& actor_;
    CoroutineFramePool::Frame* frame_;
    std::coroutine_handle<> continuation_;
};

} // namespace hpactor::sched
```

### 2.4 TimerAwaiter

For `co_await scheduler.schedule_after(delay)`:

```cpp
// include/hpactor/sched/timer_awaiter.hpp
namespace hpactor::sched {

class TimerAwaiter {
public:
    explicit TimerAwaiter(int64_t delay_ns,
                          HybridScheduler& scheduler) noexcept;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        timer_id_ = scheduler_.schedule_timer(
            delay_ns_,
            [this] { continuation_.resume(); }
        );
    }

    void await_resume() noexcept {
        // Timer fired - continuation was resumed
    }

    bool await_cancel() noexcept {
        return scheduler_.cancel_timer(timer_id_);
    }

private:
    HybridScheduler& scheduler_;
    int64_t delay_ns_;
    uint64_t timer_id_;
    std::coroutine_handle<> continuation_;
};

} // namespace hpactor::sched
```

---

## 3. Integration with HybridScheduler

### 3.1 Actor Registration Flow

```
Actor spawns
    │
    ├─ Create ActorId
    │
    ├─ Create MPSCMailbox<MessageNode>
    │
    ├─ Create CoroutineTask via actor's entry point
    │   └─ CoroutineTask holds coroutine_handle<CoroutinePromise>
    │   └─ Promise is in Idle state (initial_suspend called)
    │
    └─ Register with ActorSystem
            └─ Add to actor registry
```

### 3.2 Message Delivery to Scheduling

```
Producer sends message
    │
    ├─ Push to MPSCMailbox (MPSC enqueue)
    │
    └─ CAS mailbox.idle_ from true → false (edge-trigger)
            │
            └─ scheduler.notify_ready(actor_id, priority, deadline)
                    │
                    ├─ Upsert to EDF queue (if deadline)
                    │
                    └─ Push WorkItem to target worker's deque
```

### 3.3 Worker Execution Loop (Revised)

```cpp
void HybridScheduler::worker_loop(uint32_t worker_id) {
    WorkerThread& worker = workers_[worker_id];

    while (running_.load(std::memory_order_acquire)) {
        WorkItem item;

        // 1. Try local pop (owner - wait-free)
        if (pop_local(item, worker_id)) {
            execute_actor(item, worker);
            continue;
        }

        // 2. Try EDF (deadline-ordered)
        if (pop_edf(item, worker_id)) {
            execute_actor(item, worker);
            continue;
        }

        // 3. Try stealing via A2WS
        if (try_steal(item)) {
            execute_actor(item, worker);
            continue;
        }

        // 4. No work - backoff
        backoff();
    }
}

void HybridScheduler::execute_actor(const WorkItem& item,
                                     WorkerThread& worker) {
    auto actor = system_.get_actor(item.actor);
    if (!actor) return;

    auto* coroutine = actor->get_coroutine();
    if (!coroutine) return;

    // State: Ready → Running
    coroutine->set_state(ActorState::Running);
    coroutine->set_owner(&worker);

    // Resume the coroutine
    coroutine->handle().resume();

    // When we return here, one of:
    // - Coroutine suspended (co_await) → state is Idle or IOWaiting
    // - Coroutine finished → state is Terminated

    // Check if mailbox has more work
    if (coroutine->mailbox_has_messages()) {
        // Re-enqueue for immediate processing
        enqueue(item.actor, item.priority);
    }
}
```

---

## 4. CoroutineFramePool Integration

### 4.1 When Frames Are Needed

The `CoroutineFramePool` provides pre-allocated stack memory for **stackful** coroutines. In HPActor:

- Event-based actors use **stackless** coroutines (`co_await`, `co_return`) — no frame pool needed
- Blocking actors may use **stackful** coroutines for deep call stacks

### 4.2 Frame Acquisition/Release

```cpp
class WorkerThread {
public:
    CoroutineFramePool::Frame* acquire_frame() {
        if (frame_pool_) {
            return frame_pool_->acquire();
        }
        return nullptr;  // Pool exhausted - fall back to heap
    }

    void release_frame(CoroutineFramePool::Frame* frame) {
        if (frame_pool_ && frame) {
            frame_pool_->release(frame);
        }
    }

private:
    CoroutineFramePool* frame_pool_{nullptr};
};
```

### 4.3 Frame Lifecycle

```
Actor requests blocking operation
    │
    ├─ WorkerThread::acquire_frame()
    │       └─ CoroutineFramePool::acquire() → Frame*
    │
    ├─ Create stackful coroutine on Frame's stack
    │
    ├─ Execute operation
    │
    └─ On completion:
            ├─ Destroy stackful coroutine
            └─ WorkerThread::release_frame(frame)
                    └─ CoroutineFramePool::release(frame)
```

---

## 5. Timing Wheel Integration

### 5.1 Timer Categories

| Timer Type | Use Case | Implementation |
|------------|----------|----------------|
| Actor timeout | `co_await schedule_after(delay)` | `TimerAwaiter` → `TimingWheel::schedule()` |
| Periodic actor | `co_await schedule_every(interval)` | Recurring timer entry |
| Actor watchdog | Supervision timeout | Per-supervisor timer |
| Delayed message | `send_after(target, msg, delay)` | Message with built-in timer |

### 5.2 Timer Callback Safety

```cpp
// Timer callback runs on timer thread, but must be thread-safe with scheduler
void TimerCallback::operator()() {
    // TimerWheel callbacks are serialized (single timer thread)
    // We can safely call scheduler.notify_ready() which is documented
    // as callable from any thread
    scheduler_.notify_ready(actor_id, priority, deadline);
}
```

---

## 6. Actor Lifecycle State Transitions

### 6.1 State Encoding

```cpp
struct ActorStateWord {
    static constexpr uint32_t kIdle       = 0x01;
    static constexpr uint32_t kReady      = 0x02;
    static constexpr uint32_t kRunning    = 0x04;
    static constexpr uint32_t kIOWaiting  = 0x08;
    static constexpr uint32_t kTerminated = 0x10;
    static constexpr uint32_t kMask       = 0x1F;

    std::atomic<uint32_t> state_{kIdle};

    bool cas_state(uint32_t expected, uint32_t desired) {
        return state_.compare_exchange_strong(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
    }
};
```

### 6.2 Transition Table

| Current State | Event | New State | Action |
|--------------|-------|----------|--------|
| Idle | First message enqueued | Ready | Push to scheduler queue |
| Ready | Worker picks up | Running | Resume coroutine |
| Running | `co_await` (mailbox empty) | Idle | Suspend, notify scheduler |
| Running | `co_await` (I/O) | IOWaiting | Register with EventLoop |
| Running | `co_return` or error | Terminated | Cleanup, notify exit |
| IOWaiting | I/O completes | Ready | Push to scheduler queue |
| Ready | Worker picks up | Running | Resume coroutine |

---

## 7. Class Hierarchy Summary

```
hpactor::sched
├── IScheduler                      # Interface (testable)
│       │
│       └── HybridScheduler         # Work-stealing + EDF + A2WS
│               │
│               ├── WorkerThread[N] # Per-thread worker
│               │       │
│               │       ├── MultiPriorityWorkQueue  # Chase-Lev deques
│               │       │
│               │       └── CoroutineFramePool*     # Thread-local slab
│               │
│               ├── EDFQueue        # Global deadline index
│               │
│               ├── A2WS            # Adaptive victim selection
│               │
│               └── TimingWheel     # Hierarchical timers
│
├── CoroutineTask                   # Coroutine handle wrapper
│       │
│       └── CoroutinePromise       # Promise type for actors
│
├── MailboxAwaiter                  # co_await mailbox.receive()
│
└── TimerAwaiter                    # co_await schedule_after()
```

---

## 8. Implementation Phases

### Phase 1: CoroutineTask Foundation
- [ ] Define `CoroutinePromise` and `CoroutineTask`
- [ ] Implement `ActorStateWord` with atomic transitions
- [ ] Basic suspend/resume without scheduling

### Phase 2: MailboxAwaiter
- [ ] Implement `MailboxAwaiter::await_suspend()`
- [ ] Edge-trigger: detect when mailbox transitions empty→non-empty
- [ ] Integration with `MPSCMailbox`

### Phase 3: Scheduler Integration
- [ ] Wire `HybridScheduler::execute_actor()`
- [ ] Connect `notify_ready` → `WorkItem` → worker deque
- [ ] Worker loop: pick up and execute coroutines

### Phase 4: Timer Integration
- [ ] Implement `TimerAwaiter`
- [ ] Connect `TimingWheel` callbacks to `notify_ready`
- [ ] Support `schedule_after` and `schedule_every`

### Phase 5: Frame Pool Integration
- [ ] Thread-local `CoroutineFramePool` per `WorkerThread`
- [ ] Support stackful coroutines for blocking actors
- [ ] Fallback to heap when pool exhausted

---

## 9. Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `include/hpactor/sched/coroutine_task.hpp` | Create | `CoroutineTask`, `CoroutinePromise` |
| `include/hpactor/sched/coroutine_awaiters.hpp` | Create | `MailboxAwaiter`, `TimerAwaiter` |
| `include/hpactor/sched/actor_state.hpp` | Create | `ActorStateWord`, state constants |
| `src/sched/coroutine_task.cpp` | Create | Promise implementation |
| `src/sched/scheduler.cpp` | Modify | Add `execute_actor()` |
| `include/hpactor/sched/scheduler.hpp` | Modify | Update `HybridScheduler` |
| `include/hpactor/actor/event_based_actor.hpp` | Modify | Add coroutine integration |

---

## 10. Open Questions

1. **Stack Size**: What should `CoroutineFramePool::stack_size_` be? (8KB minimum for stackless, larger for stackful blocking actors)
2. **Exception Handling**: How to propagate errors from terminated coroutines to supervisors?
3. **Actor Migration**: Should actors ever migrate between workers, or stay with their initial owner?
4. **I/O Integration**: How does `IOWaiting` interact with the existing `EventLoop`?

---

## References

- [High-Level Design](../architecture/scheduling/scheduling-architecture-design.md)
- [Scheduling Architecture](../architecture/scheduling/scheduling-architecture-design.md)
- [Existing CoroutineFramePool](../../include/hpactor/sched/coroutine_frame_pool.hpp)
- [Existing HybridScheduler](../../include/hpactor/sched/scheduler.hpp)
- [Existing TimingWheel](../../include/hpactor/sched/timing_wheel.hpp)
