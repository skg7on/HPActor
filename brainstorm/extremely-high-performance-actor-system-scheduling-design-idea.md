Here is the English version:

---

The key conclusion is:

**For an extremely high-performance Actor system, the best general-purpose design is not “letting threads poll all Actors.” Instead, it should use an activation-based work-stealing scheduler, per-Actor MPSC mailboxes, and EventCount/Futex-based worker parking.**

The core principle is:

> **Messages go into Mailboxes, but the scheduler queue only contains Actor pointers. An Actor is enqueued only once when it transitions from idle to runnable.**
> Under bursty traffic, Workers continuously execute runnable Actors without wasting CPU on queue waiting. Under low traffic, Workers sleep via futex/eventcount so that CPUs can enter idle state.

---

## 1. Overall Architecture

```text
                send(msg)
 Producer ───────────────────► Actor.mailbox  MPSC
                                      │
                                      │ idle -> scheduled
                                      ▼
                              Worker Run Queue
                                      │
                                      ▼
                              Worker Thread
                                      │
                                      ▼
                              actor.handle(msg)
```

Each Actor owns:

```cpp
struct Actor {
    Mailbox mailbox;                 // MPSC, multi-producer single-consumer
    std::atomic<uint8_t> state;       // IDLE / SCHEDULED / RUNNING
    uint32_t home_worker;             // cache affinity
    uint32_t priority;
    ActorVTable* vtable;
};
```

Actor state machine:

```text
IDLE ──send(msg)──► SCHEDULED ──worker picks──► RUNNING
 ▲                                                   │
 │                                                   │ mailbox empty
 └───────────────────────────────────────────────────┘

RUNNING ──budget exhausted & mailbox non-empty──► SCHEDULED
```

Key points:

1. **The Mailbox receives messages.**
2. **The Run Queue stores Actors, not Messages.**
3. **A single Actor can be executed by at most one Worker at any time.**
4. **No matter how many messages an Actor receives, it produces only one runnable activation.**

This avoids two major performance problems:

* Enqueuing every message into a global scheduler queue;
* Multiple threads competing for the same Actor mailbox.

---

## 2. Scheduling Algorithm: Cache-Affine Work Stealing + Activation Coalescing

Recommended algorithm:

> **Per-worker local run queue + Actor activation coalescing + bounded mailbox draining + work stealing + futex/eventcount parking**

### Why not use a global queue?

A global queue means:

```text
N Workers concurrently push/pop the same global MPMC Queue
```

Under high throughput, this causes:

* Global queue cache-line bouncing;
* CAS contention;
* NUMA remote memory access;
* Better load balancing, but poor locality.

A better design is:

```text
Worker 0: local run queue
Worker 1: local run queue
Worker 2: local run queue
...
Worker N-1: local run queue
```

Each Actor is normally bound to a `home_worker`, usually derived from hashing the Actor ID:

```cpp
actor.home_worker = hash(actor_id) % worker_count;
```

When a message is sent, if the Actor transitions from `IDLE` to `SCHEDULED`, the Actor is pushed into its home Worker’s run queue.

```cpp
void schedule(Actor* a) {
    Worker& w = workers[a->home_worker];
    w.inject_queue.push(a);
    wake_one_worker_if_needed();
}
```

Only when a Worker has no local work does it steal work from other Workers.

```text
local pop -> drain inject queue -> steal -> spin -> park
```

This achieves:

* Cache locality;
* Work conservation;
* Full Worker utilization under bursty load;
* Worker parking under low load.

---

## 3. Actor Scheduling State Machine

Use three states:

```cpp
enum ActorState : uint8_t {
    IDLE      = 0,
    SCHEDULED = 1,
    RUNNING   = 2
};
```

### Sending a message

```cpp
void send(Actor* a, Message* msg) {
    a->mailbox.push(msg);

    uint8_t expected = IDLE;
    if (a->state.compare_exchange_strong(
            expected,
            SCHEDULED,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        enqueue_actor(a);
    }
}
```

Meaning:

* If the Actor was `IDLE`, it needs to be scheduled;
* If the Actor was already `SCHEDULED`, it is already in a run queue;
* If the Actor is `RUNNING`, a Worker is already executing it, so no additional scheduling is needed.

This is the core performance mechanism:

**During message bursts, only the first message triggers scheduling. Subsequent messages only enter the Mailbox.**

---

## 4. Worker Execution of an Actor

When a Worker picks an Actor:

```cpp
void run_actor(Actor* a, Worker* self) {
    uint8_t expected = SCHEDULED;
    if (!a->state.compare_exchange_strong(
            expected,
            RUNNING,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    uint32_t processed = 0;

    while (processed < MAX_BATCH) {
        Message* msg = a->mailbox.pop();
        if (!msg) {
            break;
        }

        a->vtable->handle(a, msg);
        ++processed;
    }

    if (a->mailbox.maybe_non_empty()) {
        a->state.store(SCHEDULED, std::memory_order_release);
        self->local_queue.push(a);
        return;
    }

    a->state.store(IDLE, std::memory_order_release);

    /*
     * Race handling:
     * A producer may push a message while the Actor is RUNNING.
     * It will not schedule the Actor because state != IDLE.
     * Therefore, after publishing IDLE, the Worker must check
     * the mailbox again.
     */
    if (a->mailbox.maybe_non_empty()) {
        uint8_t expected = IDLE;
        if (a->state.compare_exchange_strong(
                expected,
                SCHEDULED,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            self->local_queue.push(a);
        }
    }
}
```

The final recheck is critical; otherwise, wakeups may be lost.

---

## 5. Mailbox Design

For a general high-performance Actor system, use the following choices:

| Scenario                                                     | Mailbox Design                                                             |
| ------------------------------------------------------------ | -------------------------------------------------------------------------- |
| Many Actors, M far larger than N, messages from many threads | **Per-Actor intrusive MPSC linked queue**                                  |
| Few extremely hot Actors with bounded capacity               | **Bounded MPSC ring buffer**                                               |
| Single producer to a single Actor                            | **SPSC ring buffer**                                                       |
| Extremely hot Actor requiring parallelism                    | Shard the Actor instead of letting multiple threads execute the same Actor |

The best default choice is:

> **Per-Actor intrusive MPSC queue + per-thread slab allocator + batch draining**

---

## 5.1 MPSC Intrusive Linked Queue

There are multiple Producers, but only one Consumer: the Worker currently executing the Actor.

```cpp
struct Message {
    std::atomic<Message*> next;
    uint32_t type;
    uint32_t size;
    // payload follows
};

struct MpscMailbox {
    std::atomic<Message*> tail;
    Message* head;
    Message stub;
};
```

Initialization:

```cpp
void init(MpscMailbox* q) {
    q->stub.next.store(nullptr, std::memory_order_relaxed);
    q->head = &q->stub;
    q->tail.store(&q->stub, std::memory_order_relaxed);
}
```

Push:

```cpp
void push(MpscMailbox* q, Message* n) {
    n->next.store(nullptr, std::memory_order_relaxed);

    Message* prev = q->tail.exchange(n, std::memory_order_acq_rel);
    prev->next.store(n, std::memory_order_release);
}
```

The push path is very short:

```text
1 atomic exchange + 1 release store
```

No lock and no CAS retry loop.

---

## 5.2 Consumer Pop

```cpp
Message* pop(MpscMailbox* q) {
    Message* head = q->head;
    Message* next = head->next.load(std::memory_order_acquire);

    if (next != nullptr) {
        q->head = next;
        return next;
    }

    Message* tail = q->tail.load(std::memory_order_acquire);

    if (head != tail) {
        /*
         * The Producer has already exchanged tail,
         * but has not yet executed prev->next.store.
         * This is a transient inconsistent state in the MPSC queue.
         */
        return RETRY;
    }

    return nullptr;
}
```

In an actual implementation, do not treat `RETRY` as empty. Otherwise, there can be rare message latency spikes.

```cpp
Message* msg = mailbox.pop();

if (msg == RETRY) {
    cpu_relax();
    continue;
}

if (msg == nullptr) {
    break;
}
```

---

## 6. Why MPSC Linked Queue Is Usually Better Than MPMC Queue

Actor semantics naturally guarantee:

```text
The same Actor has only one consumer at any time.
```

Therefore, the Mailbox does not need to be MPMC.

An MPMC Queue introduces extra costs:

* CAS contention among consumers;
* Slot sequence checks;
* More complex memory ordering;
* Both head and tail are contended by multiple threads.

Actor Mailboxes should exploit the semantic constraint and reduce the queue to:

```text
multi-producer, single-consumer
```

This is faster than a general MPMC queue.

---

## 7. Bounded MPSC Ring Buffer Variant

If message capacity is bounded and better cache locality is required, use a bounded ring:

```cpp
struct Slot {
    std::atomic<uint64_t> seq;
    Message msg;
};

struct MpscRing {
    Slot* slots;
    uint64_t mask;

    std::atomic<uint64_t> tail;   // producers fetch_add
    uint64_t head;                // single consumer
};
```

Producer:

```cpp
bool push(MpscRing* q, Message msg) {
    uint64_t pos = q->tail.fetch_add(1, std::memory_order_acq_rel);
    Slot* s = &q->slots[pos & q->mask];

    while (s->seq.load(std::memory_order_acquire) != pos) {
        cpu_relax();
    }

    s->msg = msg;
    s->seq.store(pos + 1, std::memory_order_release);
    return true;
}
```

Consumer:

```cpp
bool pop(MpscRing* q, Message* out) {
    uint64_t pos = q->head;
    Slot* s = &q->slots[pos & q->mask];

    if (s->seq.load(std::memory_order_acquire) != pos + 1) {
        return false;
    }

    *out = s->msg;
    s->seq.store(pos + q->mask + 1, std::memory_order_release);
    q->head = pos + 1;
    return true;
}
```

Advantages:

* Contiguous memory;
* Good cache locality;
* No dynamic allocation;
* Very fast consumer path.

Disadvantages:

* Bounded capacity;
* Producers need backpressure when the queue is full;
* Multiple producers still contend on `tail.fetch_add`.

Recommended choices:

```text
Default: intrusive MPSC linked queue
Extremely hot Actor: bounded MPSC ring
Single-producer Actor: SPSC ring
```

---

## 8. Worker Thread Design

Worker main loop:

```cpp
void worker_loop(Worker* self) {
    while (!shutdown) {
        Actor* a = nullptr;

        a = self->local_queue.pop();
        if (a) {
            run_actor(a, self);
            continue;
        }

        a = self->drain_inject_queue();
        if (a) {
            run_actor(a, self);
            continue;
        }

        a = steal_from_other_workers(self);
        if (a) {
            run_actor(a, self);
            continue;
        }

        idle_or_park(self);
    }
}
```

Priority order:

```text
1. Local runnable Actor
2. Actor injected into this Worker
3. Steal from other Workers
4. Spin/backoff
5. Futex park
```

---

## 9. Worker Run Queue

Recommended structure:

```text
Worker {
    LocalDeque local_queue;     // owner fast path
    MpscQueue  inject_queue;    // other threads schedule into this worker
    uint64_t   rng_state;       // random victim stealing
}
```

### Local queue

The Worker itself pushes/pops; other Workers steal.

Use a Chase-Lev deque:

```text
owner push bottom
owner pop bottom
thief steal top
```

For throughput, owner-side LIFO improves cache locality. For fairness, FIFO or batch rotation can be used.

Recommended strategy:

```text
Requeue local Actor: push bottom
Stealing: steal top
External injection: enter inject queue first; Worker drains it into local_queue in batches
```

This prevents multiple Producers from directly contending on the local deque hot path.

---

## 10. Actor Execution Budget

A hot Actor must not monopolize a Worker indefinitely, otherwise other Actors may starve.

Use dual budgets:

```cpp
constexpr uint32_t MAX_BATCH = 64;        // message-count budget
constexpr uint64_t MAX_NS    = 20'000;    // time budget, e.g. 20 us
```

Execution logic:

```cpp
while (processed < MAX_BATCH &&
       now() - start < MAX_NS) {
    msg = mailbox.pop();
    if (!msg) break;
    handle(msg);
}
```

Throughput-oriented configuration:

```text
MAX_BATCH = 64 / 128 / 256
```

Low-tail-latency configuration:

```text
MAX_BATCH = 8 / 16 / 32
```

Adaptive policy:

```text
If local queue is empty, continue draining the current Actor.
If local queue is non-empty, requeue the current Actor after the batch expires.
```

Pseudo-code:

```cpp
if (mailbox.non_empty()) {
    if (self->local_queue.empty()) {
        continue_running_same_actor();
    } else {
        requeue_actor();
    }
}
```

This balances throughput and fairness.

---

## 11. Worker Parking / Wakeup Design

Under low load, Workers should not busy-spin forever. Use:

```text
short spin -> yield/backoff -> futex/eventcount park
```

Do not put `std::condition_variable` on the hot path. It usually introduces a mutex, kernel transitions, and heavier wakeup overhead.

### EventCount model

Globally maintain:

```cpp
struct Scheduler {
    std::atomic<uint64_t> event_seq;
    std::atomic<uint32_t> sleeping_workers;
    std::atomic<uint32_t> runnable_actors;
};
```

When scheduling an Actor:

```cpp
void enqueue_actor(Actor* a) {
    Worker& w = workers[a->home_worker];

    w.inject_queue.push(a);

    scheduler.runnable_actors.fetch_add(1, std::memory_order_relaxed);

    scheduler.event_seq.fetch_add(1, std::memory_order_release);

    if (scheduler.sleeping_workers.load(std::memory_order_acquire) > 0) {
        futex_wake_one(&scheduler.event_seq);
    }
}
```

When a Worker becomes idle:

```cpp
void idle_or_park(Worker* self) {
    for (int i = 0; i < SPIN_LIMIT; ++i) {
        cpu_relax();

        if (has_local_or_global_work(self)) {
            return;
        }
    }

    uint64_t seq = scheduler.event_seq.load(std::memory_order_acquire);

    scheduler.sleeping_workers.fetch_add(1, std::memory_order_acq_rel);

    /*
     * Prevent lost wakeups:
     * after publishing sleeping state, the Worker must recheck work.
     */
    if (has_local_or_global_work(self)) {
        scheduler.sleeping_workers.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    futex_wait(&scheduler.event_seq, seq, timeout);

    scheduler.sleeping_workers.fetch_sub(1, std::memory_order_acq_rel);
}
```

This satisfies:

```text
Burst traffic: enqueue -> wake worker -> worker executes immediately
Low traffic: worker futex park -> CPU idle
Periodic wakeup: futex_wait timeout -> timer/check/maintenance
```

---

## 12. Avoiding CPU Waste Under Message Bursts

Under bursty traffic, the key is to avoid repeated sleep/wakeup cycles and avoid scheduling every message.

Correct path:

```text
Producer pushes 1000 messages to the same Actor
    ↓
Actor transitions IDLE -> SCHEDULED only once
    ↓
Worker wakes once
    ↓
Worker batch-drains 64/128/256 messages
    ↓
If mailbox is still non-empty, continue execution or requeue
```

Wrong path:

```text
1000 messages -> 1000 scheduler enqueues -> 1000 wakeups -> 1000 queue operations
```

The hot-path cost in the correct design is close to:

```text
Producer: MPSC push
First message only: CAS Actor state + enqueue Actor
Consumer: batch pop + handle
```

---

## 13. Saving CPU Under Low Message Volume

Under low load, a Worker should execute:

```text
local queue empty
inject queue empty
steal failed
short spin
futex park with timeout
```

Timeout is used for periodic wakeups:

```text
100 us - 1 ms: ultra-low-latency configuration
1 ms - 10 ms: general-purpose configuration
10 ms+: low-power configuration
```

Normal message arrival does not rely on timeout. It uses:

```text
producer enqueue actor -> event_seq++ -> futex_wake_one
```

So low-load latency can still be very low.

---

## 14. NUMA and Cache-Line Design

Extreme performance requires careful memory layout.

### Put Actor state on a separate cache line

```cpp
struct alignas(64) ActorRuntime {
    std::atomic<uint8_t> state;
    uint8_t padding[63];
};
```

This prevents multiple hot Actors from sharing the same cache line.

### Put Worker data on separate cache lines

```cpp
struct alignas(64) Worker {
    LocalDeque local_queue;
    MpscQueue inject_queue;

    alignas(64) std::atomic<uint64_t> stats_processed;
    alignas(64) std::atomic<uint32_t> parked;
};
```

### Message allocation

Do not use general-purpose `malloc/free` on the hot path.

Recommended:

```text
per-worker slab allocator
fixed-size block per message
large payload stored as pointer/ref-counted buffer
```

Suggested message layout:

```cpp
struct Message {
    std::atomic<Message*> next;
    uint32_t type;
    uint32_t flags;
    uint32_t size;
    uint32_t source_actor;
    void* payload;
};
```

Small messages can be inlined:

```cpp
struct SmallMessage {
    std::atomic<SmallMessage*> next;
    uint32_t type;
    uint32_t size;
    std::byte payload[48];
};
```

The goal is to keep common messages within one cache line.

---

## 15. Backpressure

A high-throughput system must handle mailbox explosion.

Possible strategies:

### 15.1 Bounded mailbox

```text
push failure -> return EAGAIN / drop / block / spillover
```

Suitable for real-time systems.

### 15.2 Soft limit

```cpp
if (mailbox.approx_depth() > HIGH_WATERMARK) {
    sender->throttle();
}
```

Suitable for server-side systems.

### 15.3 Priority mailbox

A complex priority queue is not recommended by default because it destroys cache locality.

A better approach is multiple mailbox lanes:

```text
high
normal
low
```

The consumer drains them with weights:

```text
high:normal:low = 8:4:1
```

---

## 16. Hot Actor Problem

The fundamental limitation of the Actor model is:

```text
A single Actor cannot be executed by multiple Workers in parallel.
```

If an Actor becomes the bottleneck, the scheduler cannot magically fix it. The correct solution is sharding:

```text
Actor A
  ├── A shard 0
  ├── A shard 1
  ├── A shard 2
  └── A shard 3
```

Route by key:

```cpp
shard = hash(key) % shard_count;
send(actor_shard[shard], msg);
```

Only then can one logical service scale across multiple Workers.

---

## 17. Final Recommended Implementation

For maximum `messages/sec`, use:

```text
Scheduler:
    activation-based scheduling
    per-worker local deque
    per-worker MPSC inject queue
    randomized work stealing
    actor home-worker affinity
    bounded batch execution
    eventcount/futex parking

Mailbox:
    default: intrusive MPSC linked queue
    hot bounded path: MPSC ring buffer
    SPSC fast path for single-producer Actor
    per-worker slab allocator
    batch drain

Worker:
    pinned thread
    local pop first
    inject batch drain second
    random steal third
    spin then futex park
    wake_one on new activation
    wake_many only when runnable backlog > active workers
```

---

## 18. Performance-Critical Paths

### Producer hot path

```text
message init
mailbox.push(msg)
CAS Actor IDLE -> SCHEDULED only if needed
enqueue Actor pointer only once
```

### Consumer hot path

```text
pop Actor pointer
CAS SCHEDULED -> RUNNING
batch pop messages
handle messages
requeue or IDLE
```

### Parking hot path

```text
empty local
empty inject
steal failed
short spin
publish sleeping
recheck work
futex wait
```

---

## 19. One-Sentence Design Principle

> **The Mailbox is the message buffer; the Run Queue is the Actor activation queue. Schedule Actors, not messages. Workers prefer local work, then steal, then park. Wakeups are driven by runnable-state transitions, not by individual messages.**

This is the most robust high-performance design for an M:N Actor system across throughput, latency, and low-idle-CPU requirements.
