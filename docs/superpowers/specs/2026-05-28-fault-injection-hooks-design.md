# Deterministic Fault Injection Hooks Design

## 1. Overview

Add a deterministic fault injection framework to HPActor enabling controlled
failure simulation across all subsystems. Faults are injected via named hook
points with pre-computed schedules, making every failure reproducible from a
saved seed.

This design covers the comprehensive expansion of fault injection coverage
from the initial 3 wired sites (mailbox enqueue/dequeue, transport send) to
~117 sites across all 13 subsystems, organized in three tiers: core
message/resource path, resilience path, and observability path.

### 1.1 Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Integration model | Runtime opt-in, disabled by default | Allows staging/canary testing without recompilation; predictable branch when disabled |
| Determinism model | Pre-computed fault schedule | Strongest reproducibility — no framework-internal RNG |
| Hook naming | Hierarchical dot-separated paths with wildcards | Enables scoped enable/disable (`hpactor.transport.*`) |
| Schedule API | C++ programmatic API | Natural fit for unit tests, matches SchedulerTestDriver pattern |
| Tick model | Per-domain counters | Each subsystem advances independently; avoids coupling |
| Fault actions | Fail, Drop, Delay, Corrupt, Panic | Covers all failure modes needed for chaos testing |
| Hook mechanism | Per-subsystem fault points with global registry vector | Minimal intrusion (1-line macro), supports discovery and enumeration |
| Thread safety | Per-thread FaultController instances | Each thread has independent tick counters and schedule cursor; no contention on check() |
| Domain count | 14 domains | Fine-grained tick control: RPC, Supervision, Discovery, Tracing, Metrics get independent timelines |
| Fault observability | Structured log emission on each fire | Test-assertable timeline via MemoryLogSink |

### 1.2 Goals

1. Inject controlled failures into all 13 subsystems: mailbox, transport,
   scheduler, allocator, storage, timer, gossip, config, actor lifecycle,
   supervision, RPC, discovery, tracing, metrics, and CLI.
2. Reproduce any failing test from a saved seed and schedule checksum.
3. Enumerate all registered fault points from CLI for operator visibility.
4. Keep disabled-overhead to a single predictable branch per injection site.
5. Support multi-threaded fault injection in scheduler workers, transport I/O
   threads, and gossip protocol threads.
6. Enable chaos scenario generation via probability expansion helper.
7. Emit structured log entries on each fault fire for test assertion.

### 1.3 Non-Goals

- Running fault injection in production builds without explicit opt-in.
- Replacing existing delivery-semantics, DLQ, or supervision tests.
- Network proxy or multi-process chaos harness (that's TST-002).
- Multi-process chaos harness (`ClusterHarness`, `NetworkProxy`).
- Soak test runner with memory/latency trend tracking.
- Fuzz test corpus management.
- Protocol/config compatibility matrix.
- Performance regression benchmarks.

## 2. Core Components

### 2.1 FaultPoint

A named injection site registered at startup. Each `FaultPoint` carries:

- `path` — hierarchical dot-separated name (e.g., `"hpactor.mailbox.enqueue.fail"`)
- `domain` — the `FaultDomain` enum value
- `description` — human-readable string for CLI listing

FaultPoints self-register into a global static registry via file-scope static
registrar objects (same pattern as `TomlSystemParserRegistration<T>`). The
constructor appends to the registry during static initialization, before `main()`.
The registry is immutable at runtime once populated.

### 2.2 FaultDomain

Each subsystem gets its own tick counter. The expansion adds 5 new domains
for a total of 14:

```
FaultDomain       Tick source                     Example paths
────────────────────────────────────────────────────────────────
kMailbox (0)      enqueue() / dequeue() call       hpactor.mailbox.enqueue.fail
kTransport (1)    send() / recv() / connect()      hpactor.transport.send.drop
kScheduler (2)    run_one_ready() completion       hpactor.scheduler.worker.pause
kAllocator (3)    allocate() call                  hpactor.allocator.oom
kStorage (4)      read / write / flush call        hpactor.storage.write.fail
kTimer (5)        timer fire / cancel              hpactor.timer.schedule.drop
kGossip (6)       gossip packet send / recv        hpactor.gossip.packet.loss
kConfig (7)       config parse / reload            hpactor.config.reload.fail
kActor (8)        actor handler / lifecycle        hpactor.actor.handler.delay
kRpc (9)          RPC send / response / timeout    hpactor.rpc.send.drop
kSupervision (10) restart_child / on_failure       hpactor.supervision.restart.drop
kDiscovery (11)   registrar / location cache       hpactor.discovery.heartbeat.drop
kTracing (12)     span start / finish / export     hpactor.tracing.start_span.drop
kMetrics (13)     metric push / aggregate / drain  hpactor.metrics.ring_buffer.push.fail
```

**Why separate domains:** Each subsystem has its own tick cadence — RPC retries
fire on different timelines than raw transport sends; supervision restarts are
driven by failure events, not message counts; discovery heartbeats are
wall-clock periodic. Without separate domains, a fault scheduled for "tick 5
of kTransport" would ambiguously match transport sends, RPC sends, connection
pool operations, and registrar messages simultaneously. Independent domains
give test authors precise control.

### 2.3 FaultAction

Five actions a fault can trigger:

| Action | Behavior | Return semantics |
|--------|----------|------------------|
| `Fail` | Return a predefined failure result | `EnqueueResult::failure()`, `error::Code`, `nullptr`, etc. |
| `Drop` | Silently discard — return success | Returns success/true but performs no work |
| `Delay` | Pause caller for N domain ticks | Advances domain ticks (may trigger additional faults); falls through |
| `Corrupt` | Modify in-flight data before pass-through | Returns success, but data is altered |
| `Panic` | `std::abort()` | Does not return |

### 2.4 FaultSchedule

An ordered list of `FaultScheduleEntry`:

```
FaultScheduleEntry {
    FaultDomain domain;
    uint64_t    at_tick;       // fire on this domain tick
    std::string path;          // exact fault point path (no wildcards)
    FaultAction action;
    std::optional<ActorId> target;  // optional: only fire for this actor
    // Action-specific payload:
    //   Fail: error code or FailureReason
    //   Delay: number of domain ticks to stall
    //   Corrupt: byte offset and mask
}
```

The schedule is pre-computed by the test before actors start. The framework
has no internal RNG — it only checks the schedule. Tests may use a seeded PRNG
to generate the schedule, but once loaded it is a fixed, deterministic list.

**Schedule API:**

```cpp
FaultSchedule schedule;
schedule
    .at_domain_tick(FaultDomain::kMailbox, 0)
        .fail("hpactor.mailbox.enqueue.fail", FailureReason::kMailboxFull)
    .at_domain_tick(FaultDomain::kTransport, 42)
        .drop("hpactor.transport.send.drop")
    .at_domain_tick(FaultDomain::kTransport, 100)
        .corrupt("hpactor.transport.recv.corrupt", /*offset=*/4, /*mask=*/0xFF)
    .at_domain_tick(FaultDomain::kAllocator, 5)
        .fail("hpactor.allocator.oom", error::Code::kNoMemory)
    .at_domain_tick(FaultDomain::kScheduler, 3)
        .delay("hpactor.scheduler.worker.pause", /*ticks=*/10)
    .at_domain_tick(FaultDomain::kScheduler, 50)
        .panic("hpactor.scheduler.worker.panic");
```

**expand_random() helper (new in this expansion):**

The `expand_random()` method enables test authors to populate a schedule using
a seeded PRNG rather than hand-specifying every tick. The expansion happens
before the schedule is loaded — the schedule is a fixed, deterministic list
when execution starts. The PRNG is only used during schedule construction in
the test, preserving the determinism guarantee.

```cpp
template <typename RNG>
FaultSchedule& expand_random(
    FaultDomain domain,
    std::string_view path,
    FaultAction action,
    double probability,        // 0.0 – 1.0, fire probability per tick
    uint64_t max_ticks,        // ticks 0 .. max_ticks-1
    RNG& rng,                  // std::mt19937 or similar
    FaultPayload payload = {},
    std::optional<ActorId> target = std::nullopt);
```

For each tick `t` in `[0, max_ticks)`, a uniform real draw from `[0, 1)`
determines whether to add an entry. Entries for a single path are generated
in tick order. After all `expand_random()` calls, `FaultSchedule::sort()`
orders all entries by `(domain, at_tick)`. This is called automatically by
`FaultController::load()`.

**Usage in chaos tests:**

```cpp
TEST(ChaosScenario, PartitionDuringRPCLoad) {
    constexpr uint64_t kSeed = 0xDEADBEEF;
    std::mt19937 rng(kSeed);

    FaultSchedule schedule;
    schedule
        .expand_random(FaultDomain::kTransport, "hpactor.transport.send.drop",
                        FaultAction::kDrop, 0.20, 500, rng)
        .expand_random(FaultDomain::kTransport, "hpactor.transport.recv.drop",
                        FaultAction::kDrop, 0.20, 500, rng)
        .expand_random(FaultDomain::kGossip, "hpactor.gossip.packet.loss",
                        FaultAction::kDrop, 0.30, 100, rng);

    ActorSystem system(cfg);
    system.fault_controller().load(schedule);
    system.fault_controller().enable("*");
    system.fault_controller().set_replay_seed(kSeed);

    // Same seed → same rng → same schedule → same failure.
}
```

### 2.5 FaultController

Central controller owned by `ActorSystem`. Holds the active schedule, the
enabled/disabled flag, the active scope pattern, and per-domain tick counters.

**Per-thread architecture (new in this expansion):**

The `ActorSystem` owns a single "master" controller that holds the schedule and
configuration. Per-thread instances clone the schedule on `load()` and maintain
independent tick counters and schedule cursors:

```
ActorSystem::fault_controller()  --- FaultController (master)
                                      |  holds schedule, enabled flag, scope, seed
                                      |
                ----------------------+----------------------
                |                    |                      |
                v                    v                      v
        Thread A instance    Thread B instance     Thread C instance
        (actor thread)      (worker thread)       (I/O thread)
        ticks: [A...]       ticks: [B...]         ticks: [C...]
        cursor: Na          cursor: Nb            cursor: Nc
```

**Lifecycle:**
- `ActorSystem` constructor creates the master controller
- `FaultController::install()` sets the per-thread instance pointer for the calling thread and registers it in a global list
- Threads that call FAULT_INJECT without an installed instance see `nullptr` → fault injection is a no-op on that thread
- `FaultController::load(schedule)` on the master pushes the schedule to all known per-thread instances under a static mutex
- `enable()`/`disable()`/`clear()` similarly broadcast to all instances
- `remove()` clears the calling thread's instance and unregisters it

**Per-thread instance registry:** A `static std::mutex` protects a
`static std::vector<FaultController*>` of all per-thread instances. Used
for broadcast operations (load/enable/disable/clear) and aggregate_snapshot().

**Thread safety of `check()`:**
- Each thread accesses only its own per-thread instance — no contention
- `domain_ticks_[]` and `schedule_cursor_` are per-thread, no atomics needed
- Schedule entries vector is read-only after `load()` — safe for concurrent readers
- `faults_fired_` is per-thread

**Worker thread install:** `HybridScheduler::start()` calls
`fault_controller().install()` at the top of each `worker_loop()`.
`stop()` calls `remove()` before thread exit.

**Aggregation:** `FaultControllerSnapshot` gains a `per_thread` field listing
per-thread stats. A new `aggregate_snapshot()` sums `faults_fired` and
`domain_ticks` across all instances for CLI `/fault status`.

Key methods:

```cpp
class FaultController {
public:
    // Load a schedule (replaces any existing) — broadcasts to all per-thread instances
    void load(const FaultSchedule& schedule);
    void clear();

    // Enable/disable fault injection globally or by scope pattern — broadcasts
    void enable(std::string_view scope_pattern);   // e.g., "hpactor.transport.*"
    void disable(std::string_view scope_pattern);
    bool is_enabled() const;

    // Called by FAULT_INJECT macro at each injection site
    // Returns true if a fault is scheduled for this (tick, path, target)
    // Auto-executes kPanic (std::abort); for other actions, caller handles the response
    bool check(std::string_view path,
               std::optional<ActorId> target = std::nullopt);

    // Advance a domain tick counter directly (for domains without wired sites)
    void advance_tick(FaultDomain domain);

    // Stall a domain by N ticks (used by kDelay fault action)
    void stall(FaultDomain domain, uint64_t delay_ticks);

    // Seed for replay (stored, not used internally)
    void set_replay_seed(uint64_t seed);
    uint64_t replay_seed() const;

    // Per-thread instance management (new)
    void install();    // set thread-local instance for calling thread, register in global list
    void remove();     // clear thread-local instance, unregister from global list
    static FaultController* instance();  // returns thread-local instance pointer

    // Snapshot for metrics/CLI
    FaultControllerSnapshot snapshot() const;
    FaultControllerSnapshot aggregate_snapshot() const;  // sums across all threads (new)

    uint64_t faults_fired() const noexcept;
};
```

### 2.6 FAULT_INJECT Macro

The injection site macro expands to a predictable cold branch:

```cpp
#define FAULT_INJECT(path) \
    if (auto* _fc = ::hpactor::fault::FaultController::instance(); \
        HPACTOR_UNLIKELY(_fc != nullptr && _fc->check(path)))
```

When the fault controller is disabled (per-thread instance is null), the branch
is a single predictable pointer comparison. The compiler hoists the check and
the failure-handling code stays cold.

**Wiring patterns by action type:**

*kFail* — return error/sentinel immediately:
```cpp
EnqueueResult MPSCActorMailbox::try_push(T&& msg, MailboxEnvelopeMeta meta) {
    FAULT_INJECT("hpactor.mailbox.try_push.fail") {
        return EnqueueResult::failure(FailureReason::kMailboxFull);
    }
    // ... normal fast path
}
```

*kDrop* — silently discard, return success:
```cpp
ssize_t TcpTransport::try_send(span<const uint8_t> data) {
    FAULT_INJECT("hpactor.transport.send.drop") {
        return static_cast<ssize_t>(data.size()); // lie: claim success
    }
    // ... normal fast path
}
```

*kDelay* — stall via `FaultController::stall()`, then fall through:
```cpp
void HybridScheduler::process_actor(ActorId actor) {
    FAULT_INJECT("hpactor.scheduler.process_actor.delay") {
        _fc->stall(FaultDomain::kScheduler, /*delay_ticks=*/3);
    }
    // ... normal processing continues after stall
}
```

*kCorrupt* — mutate data in place, then fall through:
```cpp
bool DedupCache::is_duplicate(const DedupKey& key) {
    bool result = lookup_impl(key);
    FAULT_INJECT("hpactor.mailbox.dedup.is_duplicate.corrupt") {
        result = !result;  // flip: duplicate ↔ new
    }
    return result;
}
```

*kPanic* — `FaultController::check()` calls `std::abort()` for kPanic; the
FAULT_INJECT body is unreachable but the macro braces are syntactically required:
```cpp
void HybridScheduler::worker_loop() {
    FAULT_INJECT("hpactor.scheduler.worker_loop.panic") {
        // unreachable — FaultController::check() already aborted
    }
    // ... normal path
}
```

**Multi-action stacking:** Multiple FAULT_INJECT macros at the same site are
independent — `check()` uses the path string to match the scheduled fault. Only
one fires per call (the first scheduled match). Stacking order should be:
Drop first (returns), then Delay, then Corrupt (both fall through):

```cpp
ssize_t TcpTransport::try_send(span<const uint8_t> data) {
    FAULT_INJECT("hpactor.transport.send.drop") {
        return static_cast<ssize_t>(data.size());
    }
    FAULT_INJECT("hpactor.transport.send.delay") {
        _fc->stall(FaultDomain::kTransport, /*delay_ticks=*/3);
    }
    FAULT_INJECT("hpactor.transport.send.corrupt") {
        if (!data.empty()) const_cast<uint8_t&>(data[0]) ^= 0xFF;
    }
    // ... normal path
}
```

### 2.7 FaultPoint Registry

A compile-time-constructed vector of all registered fault points. Each
`FaultPoint` definition registers its path into the registry via file-scope
static registrar objects:

```cpp
// In mailbox source:
namespace {
const FaultPointRegistrar kMailboxEnqueueFail{
    "hpactor.mailbox.enqueue.fail",
    FaultDomain::kMailbox,
    "Mailbox enqueue fails with capacity error"
};
} // namespace
```

The registry supports:
- Exact path lookup for `check()`
- Prefix walk for wildcard `enable("hpactor.transport.*")`
- Full enumeration for CLI `/fault list`
- Domain filtering for CLI `/fault list <domain>` (new)

### 2.8 Fault Timeline Log (new)

`FaultController::check()` emits a structured log entry each time a fault fires.
The entry is written to the existing structured logging subsystem.

**Log entry schema:**
```json
{
    "event": "fault_inject",
    "domain": "kTransport",
    "tick": 42,
    "path": "hpactor.transport.send.drop",
    "action": "Drop",
    "target": null,
    "schedule_index": 3,
    "thread_id": 7,
    "replay_seed": 3735928559
}
```

**Integration:** `FaultController` holds a `LogManager*` (set by `ActorSystem`,
nullptr if logging is disabled). A new `LogCategory::kFault` enables per-category
filtering. When `ENABLE_ACTOR_LOGGING=OFF`, log emission is a no-op.

**Test assertion pattern via MemoryLogSink:**
```cpp
auto& log_sink = system.log_manager().add_memory_sink(LogCategory::kFault);
// ... run scenario with faults ...
EXPECT_TRUE(log_sink.contains("hpactor.mailbox.enqueue.fail"));
EXPECT_TRUE(log_sink.timeline_ordered({
    "hpactor.mailbox.enqueue.fail",
    "hpactor.transport.send.drop"
}));
```

## 3. Fault Point Catalog

### 3.1 Naming Convention

`hpactor.<subsystem>.<operation>.<effect>`

| Component | Example | Meaning |
|-----------|---------|---------|
| `hpactor` | — | Fixed prefix |
| `<subsystem>` | `mailbox`, `scheduler`, `rpc` | Maps to one of 14 FaultDomain values |
| `<operation>` | `enqueue`, `notify_ready`, `start_span` | The specific method or code path |
| `<effect>` | `fail`, `drop`, `delay`, `corrupt`, `panic` | The FaultAction applied at this point |

### 3.2 Registration Pattern

Each source file registers its fault points via file-scope `FaultPointRegistrar`
objects in an anonymous namespace. All ~117 points are registered in source files
co-located with the FAULT_INJECT call sites.

### 3.3 Representative Catalog

**Tier 1 — Core Message/Resource Path (~43 sites):**

| Fault Point | Domain | Actions | What It Tests |
|---|---|---|---|
| `hpactor.mailbox.try_push.fail` | kMailbox | Fail | Admission rejection recovery |
| `hpactor.mailbox.dlq.push.drop` | kMailbox | Drop | DLQ record loss tolerance |
| `hpactor.mailbox.dedup.is_duplicate.corrupt` | kMailbox | Corrupt | Handler idempotency |
| `hpactor.mailbox.pressure_state.corrupt` | kMailbox | Corrupt | Wrong backpressure signal |
| `hpactor.mailbox.drain_overflow.fail` | kMailbox | Fail | Overflow drain stall |
| `hpactor.mailbox.enqueue_reserved.drop` | kMailbox | Drop | Message lost after capacity committed |
| `hpactor.mailbox.drop_oldest.fail` | kMailbox | Fail | Eviction failure |
| `hpactor.mailbox.overflow.push.drop` | kMailbox | Drop | Overflow message loss |
| `hpactor.transport.send.delay` | kTransport | Delay | Transport send latency |
| `hpactor.transport.send.corrupt` | kTransport | Corrupt | Wire-level payload corruption |
| `hpactor.transport.recv.drop` | kTransport | Drop | Inbound message loss |
| `hpactor.transport.recv.corrupt` | kTransport | Corrupt | Wire-level receive corruption |
| `hpactor.transport.connection.reset` | kTransport | Fail | Connection reset |
| `hpactor.connection_pool.send.drop` | kTransport | Drop | Outbound message silently dropped |
| `hpactor.connection_pool.try_send.fail` | kTransport | Fail | Admission denied |
| `hpactor.connection_pool.reconnect.drop` | kTransport | Drop | Reconnection prevented |
| `hpactor.connection_pool.reconnect.delay` | kTransport | Delay | Reconnection delayed |
| `hpactor.connection_pool.pending.drop` | kTransport | Drop | Pending queue message loss |
| `hpactor.connection_pool.flush.drop` | kTransport | Drop | Flush drain silently drops |
| `hpactor.connection_pool.frame.drop` | kTransport | Drop | Received frame dropped |
| `hpactor.connection_pool.frame.corrupt` | kTransport | Corrupt | Received frame corrupted |
| `hpactor.wireframe.handle_read.drop` | kTransport | Drop | Frame reassembly drop |
| `hpactor.wireframe.handle_read.corrupt` | kTransport | Corrupt | Frame reassembly corruption |
| `hpactor.wireframe.flush_write_buffer.drop` | kTransport | Drop | Write never transmitted |
| `hpactor.acceptor.listen.fail` | kTransport | Fail | Bind/listen failure |
| `hpactor.acceptor.accept.drop` | kTransport | Drop | Inbound connection silently dropped |
| `hpactor.scheduler.notify_ready.drop` | kScheduler | Drop | Lost actor wakeup |
| `hpactor.scheduler.notify_ready.corrupt` | kScheduler | Corrupt | Wrong worker/priority routing |
| `hpactor.scheduler.try_steal.fail` | kScheduler | Fail | Steal blindness (load imbalance) |
| `hpactor.scheduler.pop_local.fail` | kScheduler | Fail | Spurious empty local queue |
| `hpactor.scheduler.execute_actor.msg_drop` | kScheduler | Drop | Message dropped mid-processing |
| `hpactor.scheduler.execute_actor.dispatch_skip` | kScheduler | Drop | Actor dequeued but not executed |
| `hpactor.scheduler.worker_loop.exit_early` | kScheduler | Fail | Simulated thread death |
| `hpactor.scheduler.process_actor.delay` | kScheduler | Delay | Processing stall |
| `hpactor.scheduler.reenqueue_drop` | kScheduler | Drop | Actor starvation after processing |
| `hpactor.timing_wheel.schedule.fail` | kTimer | Drop | Timer never registered |
| `hpactor.timing_wheel.advance.skip` | kTimer | Drop | Expired timers never fire |
| `hpactor.timing_wheel.cancel.fail` | kTimer | Drop | Timer cancel silently ignored |
| `hpactor.allocator.oom` | kAllocator | Fail | Allocation returns nullptr |
| `hpactor.allocator.segment.mmap_fail` | kAllocator | Fail | Root OOM from OS (MAP_FAILED) |
| `hpactor.allocator.freelist.pop.corrupt` | kAllocator | Corrupt | ABA / stale next pointer |
| `hpactor.allocator.freelist.push.corrupt` | kAllocator | Corrupt | Double-free / self-referencing node |
| `hpactor.allocator.slab_cache.refill_fail` | kAllocator | Fail | Slab exhaustion |
| `hpactor.allocator.region.try_reserve.fail` | kAllocator | Fail | Hard-limit rejection |
| `hpactor.allocator.region.record_free.skip` | kAllocator | Drop | Accounting drift (active_bytes inflation) |
| `hpactor.allocator.canary.verify.corrupt` | kAllocator | Corrupt | False corruption detection |

**Tier 2 — Resilience Path (~43 sites):**

| Fault Point | Domain | Actions | What It Tests |
|---|---|---|---|
| `hpactor.actor.lifecycle.transition.fail` | kActor | Fail | State machine stuck |
| `hpactor.actor.lifecycle.transition.corrupt` | kActor | Corrupt | Transition to wrong state |
| `hpactor.actor.lifecycle.accepts_msgs.corrupt` | kActor | Corrupt | Gate lets messages through in wrong state |
| `hpactor.actor.receive.drop` | kActor | Drop | Message silently skipped |
| `hpactor.actor.receive.delay` | kActor | Delay | Slow message processing |
| `hpactor.actor.become.drop` | kActor | Drop | Behavior swap refused |
| `hpactor.actor.on_exit.drop` | kActor | Drop | DownMsg never sent to linked actors |
| `hpactor.actor.spawn.fail` | kActor | Fail | Actor creation failure |
| `hpactor.actor.spawn.corrupt` | kActor | Corrupt | Wrong actor ID or type assigned |
| `hpactor.actor.spawn.drop` | kActor | Drop | Skip registry insertion |
| `hpactor.actor.drain_one.corrupt` | kActor | Corrupt | Wrong drain policy decision |
| `hpactor.actor.circuit_breaker.record.fail` | kActor | Fail | Always trip regardless of success |
| `hpactor.supervision.restart_child.drop` | kSupervision | Drop | Child not restarted |
| `hpactor.supervision.restart_child.fail` | kSupervision | Fail | Restart count never resets, child permanently killed |
| `hpactor.supervision.handle_child_down.drop` | kSupervision | Drop | Supervisor never sees child death |
| `hpactor.supervision.handle_child_down.corrupt` | kSupervision | Corrupt | Wrong directive dispatched (Stop instead of Restart) |
| `hpactor.supervision.decide_restart.fail` | kSupervision | Fail | Always returns Stop |
| `hpactor.supervision.add_child.drop` | kSupervision | Drop | Child registration silently refused |
| `hpactor.supervision.remove_child.drop` | kSupervision | Drop | Stale child reference persists |
| `hpactor.gossip.packet.loss` | kGossip | Drop | All gossip outbound drops |
| `hpactor.gossip.ping.drop` | kGossip | Drop | False suspicion cascade |
| `hpactor.gossip.ping.delay` | kGossip | Delay | Slow liveness probe |
| `hpactor.gossip.ack.drop` | kGossip | Drop | Ack loss → false suspicion |
| `hpactor.gossip.ping_req.drop` | kGossip | Drop | Indirect probe loss |
| `hpactor.gossip.join.drop` | kGossip | Drop | Cluster formation failure |
| `hpactor.gossip.sync_rsp.corrupt` | kGossip | Corrupt | Poisoned membership table |
| `hpactor.gossip.leave.drop` | kGossip | Drop | Graceful departure invisible |
| `hpactor.gossip.protocol_round.delay` | kGossip | Delay | Slowed failure detection |
| `hpactor.gossip.mark_suspicious.drop` | kGossip | Drop | Dead node never suspected |
| `hpactor.gossip.mark_dead.drop` | kGossip | Drop | Tombstone never created |
| `hpactor.gossip.merge_member.corrupt` | kGossip | Corrupt | Incarnation corruption → split-brain |
| `hpactor.gossip.pick_random_peers.fail` | kGossip | Fail | No peers selected, gossip isolated |
| `hpactor.rpc.send.drop` | kRpc | Drop | Request never leaves |
| `hpactor.rpc.send.delay` | kRpc | Delay | Slow request transmission |
| `hpactor.rpc.send.corrupt` | kRpc | Corrupt | Request payload corrupted |
| `hpactor.rpc.response.drop` | kRpc | Drop | Response lost → caller timeout |
| `hpactor.rpc.response.delay` | kRpc | Delay | Slow response delivery |
| `hpactor.rpc.response.corrupt` | kRpc | Corrupt | Response payload corrupted |
| `hpactor.rpc.timeout.drop` | kRpc | Drop | Pending call never resolved (leak) |
| `hpactor.rpc.retry.drop` | kRpc | Drop | Retry never scheduled |
| `hpactor.discovery.heartbeat.drop` | kDiscovery | Drop | Node presumed dead |
| `hpactor.discovery.register.drop` | kDiscovery | Drop | Node never joins cluster |
| `hpactor.discovery.connect.fail` | kDiscovery | Fail | Unreachable registrar |
| `hpactor.location_cache.get.fail` | kDiscovery | Fail | Forced rediscovery |
| `hpactor.location_cache.get.corrupt` | kDiscovery | Corrupt | Wrong endpoint → misroute |
| `hpactor.location_cache.put.drop` | kDiscovery | Drop | Never cached, always slow path |
| `hpactor.location_cache.evict.drop` | kDiscovery | Drop | Stale entry persists |

**Tier 3 — Observability Path (~31 sites):**

| Fault Point | Domain | Actions | What It Tests |
|---|---|---|---|
| `hpactor.tracing.start_span.drop` | kTracing | Drop | Span silently not recorded |
| `hpactor.tracing.finish_span.drop` | kTracing | Drop | Span record lost |
| `hpactor.tracing.inject_context.corrupt` | kTracing | Corrupt | Broken traceparent propagation |
| `hpactor.tracing.parse_context.fail` | kTracing | Fail | Unparseable trace context |
| `hpactor.tracing.exporter.export.fail` | kTracing | Fail | Backend unreachable |
| `hpactor.tracing.sampler.corrupt` | kTracing | Corrupt | Sampling inversion |
| `hpactor.tracing.id_generator.duplicate` | kTracing | Corrupt | Trace/Span ID collision |
| `hpactor.tracing.drain_once.fail` | kTracing | Fail | Ring buffer drain stall |
| `hpactor.tracing.force_flush.fail` | kTracing | Fail | Shutdown hang |
| `hpactor.tracing.start.fail` | kTracing | Fail | Drain thread never spawned |
| `hpactor.metrics.ring_buffer.push.fail` | kMetrics | Fail | Metric event lost |
| `hpactor.metrics.aggregator.on_event.corrupt` | kMetrics | Corrupt | Wrong event type/code |
| `hpactor.metrics.registry.snapshot.corrupt` | kMetrics | Corrupt | Corrupted counter/gauges |
| `hpactor.metrics.formatter.format.corrupt` | kMetrics | Corrupt | Malformed OpenMetrics output |
| `hpactor.metrics.actor.process_event.drop` | kMetrics | Drop | Silent event drop |
| `hpactor.metrics.registry.register_family.fail` | kMetrics | Fail | Family type mismatch |
| `hpactor.metrics.registry.get_or_create.fail` | kMetrics | Fail | Null value storage |
| `hpactor.config.parse.fail` | kConfig | Fail | TOML parse failure propagation |
| `hpactor.config.parse.corrupt` | kConfig | Corrupt | Missing/wrong actors in model |
| `hpactor.config.actor_factory.get.fail` | kConfig | Fail | nullptr for registered type |
| `hpactor.config.binary_load.fail` | kConfig | Fail | mmap failure fallback |
| `hpactor.config.binary_load.corrupt` | kConfig | Corrupt | Corrupted string offsets |
| `hpactor.config.toml_table_view.read.corrupt` | kConfig | Corrupt | Wrong type/value returned |
| `hpactor.cli.actor.run_once.fail` | kActor | Fail | Daemon exits, tests respawn |
| `hpactor.cli.actor.inspect_request.fail` | kActor | Fail | Timeout on inspect |
| `hpactor.cli.execute_tokens.corrupt` | kActor | Corrupt | Wrong command routing |
| `hpactor.cli.command_node.find_child.fail` | kActor | Fail | Always miss (test suggestions) |
| `hpactor.cli.lexer.tokenize.corrupt` | kActor | Corrupt | Wrong tokens produced |
| `hpactor.cli.line_editor.readline.corrupt` | kActor | Corrupt | Garbage input |
| `hpactor.cli.pager.show_page.fail` | kActor | Fail | Render nothing |
| `hpactor.cli.formatter.output.corrupt` | kActor | Corrupt | Malformed ANSI/JSON output |

## 4. Test UX

### 4.1 Deterministic Fault Test (existing pattern)

```cpp
TEST(FaultInjection, MailboxFullRoutesToDLQ) {
    ActorSystemConfig cfg;
    cfg.scheduler_threads = 0;  // no scheduler, manual pump
    ActorSystem system(cfg);
    SchedulerTestDriver driver(system);

    FaultSchedule schedule;
    schedule
        .at_domain_tick(FaultDomain::kMailbox, 0)
            .fail("hpactor.mailbox.enqueue.fail", FailureReason::kMailboxFull);

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("*");
    fc.install();

    auto actor = system.spawn<TestActor>();
    auto result = system.send(actor->address(), test_message);
    // Fault fired — result reflects mailbox full
    EXPECT_EQ(result.status(), DeliveryStatus::kFailed);
    EXPECT_EQ(result.failure_reason(), FailureReason::kMailboxFull);
}
```

### 4.2 Per-Thread Deterministic Test (new)

```cpp
TEST(FaultInjection, WorkerThreadsEachSeeOwnTicks) {
    ActorSystemConfig cfg;
    cfg.scheduler_threads = 2;
    ActorSystem system(cfg);
    SchedulerTestDriver driver(system);

    FaultSchedule schedule;
    schedule.add_entry(
        add_entry_to(schedule, FaultDomain::kScheduler, 3)
            .drop("hpactor.scheduler.try_steal.fail"));

    system.fault_controller().load(schedule);
    system.fault_controller().enable("*");

    for (int i = 0; i < 10; i++) {
        driver.run_one();
    }

    auto snap = system.fault_controller().aggregate_snapshot();
    EXPECT_EQ(snap.faults_fired, 2); // once per worker
}
```

### 4.3 Chaos Scenario Test with expand_random (new)

```cpp
TEST(ChaosScenario, PartitionDuringRPCLoad) {
    constexpr uint64_t kSeed = 0xDEADBEEF;
    std::mt19937 rng(kSeed);

    FaultSchedule schedule;
    schedule
        .expand_random(FaultDomain::kTransport, "hpactor.transport.send.drop",
                        FaultAction::kDrop, 0.20, 500, rng)
        .expand_random(FaultDomain::kGossip, "hpactor.gossip.packet.loss",
                        FaultAction::kDrop, 0.30, 100, rng);

    ActorSystem system(cfg);
    system.fault_controller().load(schedule);
    system.fault_controller().enable("*");
    system.fault_controller().set_replay_seed(kSeed);

    // Run scenario — verify system converges after faults stop

    auto snap = system.fault_controller().aggregate_snapshot();
    EXPECT_GT(snap.faults_fired, 0);
}
```

### 4.4 Fault Timeline Assertion (new)

```cpp
TEST(FaultInjection, TimelineRecordsFaultOrder) {
    ActorSystem system(cfg);
    auto sink = system.log_manager().add_memory_sink(LogCategory::kFault);

    FaultSchedule schedule;
    schedule
        .add_entry(add_entry_to(schedule, FaultDomain::kMailbox, 0)
            .fail("hpactor.mailbox.enqueue.fail", FailureReason::kMailboxFull))
        .add_entry(add_entry_to(schedule, FaultDomain::kTransport, 1)
            .drop("hpactor.transport.send.drop"));

    system.fault_controller().load(schedule);
    system.fault_controller().enable("*");
    system.fault_controller().install();

    // ... trigger faults ...

    EXPECT_TRUE(sink->timeline_ordered({
        "hpactor.mailbox.enqueue.fail",
        "hpactor.transport.send.drop"
    }));
}
```

### 4.5 Integration with SchedulerTestDriver

```cpp
SchedulerTestDriver driver(system);
FaultController& fc = system.fault_controller();
fc.load(schedule);
fc.enable("*");
fc.install();  // for main test thread

// Each driver.run_one() advances the kScheduler domain tick.
// Mailbox/transport/allocator domain ticks advance independently
// as the corresponding operations are called by actors.
// Worker threads have their own per-thread instances installed
// by HybridScheduler::start().
for (int i = 0; i < 100; i++) {
    driver.run_one();
    // Faults fire deterministically at their scheduled domain ticks
}
```

### 4.6 Test File Layout

```
tests/unit/fault/
    test_fault_controller.cpp       — (existing) expanded for per-thread tests
    test_fault_schedule.cpp         — (existing) expanded for expand_random()
    test_fault_point.cpp            — (existing) expanded for new domain registrations
    test_fault_macro.cpp            — (new) macro expansion patterns for each action

tests/integration/fault/
    test_fault_mailbox.cpp          — (existing) expanded with new mailbox points
    test_fault_transport.cpp        — (existing) expanded with pool/wireframe points
    test_fault_scheduler.cpp        — (new) worker thread fault injection
    test_fault_allocator.cpp        — (new) OOM and corruption injection
    test_fault_actor_lifecycle.cpp  — (new) lifecycle/supervision faults
    test_fault_gossip.cpp           — (new) gossip protocol faults
    test_fault_rpc.cpp              — (new) RPC channel faults
    test_fault_tracing.cpp          — (new) tracing subsystem faults
    test_fault_seed_replay.cpp      — (existing) expanded with per-thread replay
    test_fault_chaos_scenario.cpp   — (new) multi-subsystem chaos tests

tests/system/fault/
    test_fault_end_to_end.cpp       — (new) full-system fault injection under load
```

All tests follow existing determinism constraints: no timing assumptions,
`scheduler_threads=0` where possible, condition-based polling with 5s+
timeouts where scheduler is needed.

## 5. Observability

### 5.1 Metrics

```
hpactor_fault_injections_total{path, action, target}  — counter, incremented per fire
hpactor_fault_schedule_entry_count                     — gauge, entries in loaded schedule
hpactor_fault_controller_enabled                       — gauge, 0 or 1
hpactor_fault_thread_count                             — gauge, number of installed per-thread instances (new)
hpactor_fault_injections_per_thread{thread_id}         — counter, per-thread fault fire count (new)
```

Integrates with existing `MpscRingBuffer` → `MetricsActor` → Prometheus path.
`MetricEventType::kFaultInjected` (value 26) is emitted by `check()` on each fire.

### 5.2 CLI Commands

Guarded by `ENABLE_CLI`:

```
/fault status                  — enabled state, scope, schedule size, replay seed (aggregate)
/fault status threads          — (new) show per-thread breakdown
/fault list                    — enumerate all registered fault points
/fault list <domain>           — (new) filter by domain
/fault enable <scope>          — enable faults matching scope pattern
/fault disable <scope>         — disable faults matching scope pattern
/fault clear                   — unload schedule, disable all
/fault seed [<value>]          — display or set the replay seed
/fault timeline [<domain>]     — (new) show recent fault fire log entries
/fault timeline tail           — (new) continuous tail of fault events
```

## 6. Build Integration

### 6.1 CMake Option

```cmake
option(ENABLE_FAULT_INJECTION "Enable deterministic fault injection hooks" ON)
```

When `ENABLE_FAULT_INJECTION=OFF`, the `FAULT_INJECT` macro expands to `if (false)`
and the `FaultController`, `FaultPoint`, `FaultSchedule` headers are still
included (the types exist) but all injection sites are dead code eliminated.
This keeps the API stable for downstream code while removing all overhead.

When `ENABLE_FAULT_INJECTION=ON` (default), the runtime is compiled with full
fault injection support but disabled by default. Tests compile against the same
library and enable faults explicitly.

### 6.2 File Layout

```
include/hpactor/fault/
    fault_controller.hpp          — FaultController class (modified: per-thread, 14 domains)
    fault_schedule.hpp            — FaultSchedule, FaultScheduleEntry, expand_random(), sort()
    fault_point.hpp               — FaultPoint, FaultDomain, fault point registration
    fault_macros.hpp              — FAULT_INJECT macro (unchanged)
    fault_types.hpp               — FaultDomain enum (14 values), FaultAction

src/fault/
    fault_controller.cpp          — FaultController implementation (per-thread, timeline log)
    fault_schedule.cpp            — FaultSchedule builder, expand_random() implementation
    fault_point_registry.cpp      — Global registry, FaultPoint self-registration
    fault_points.cpp              — ~117 FaultPointRegistrar objects (organized by subsystem)

Source modifications (representative):
    src/mem/memory_config.cpp         — FAULT_INJECT("hpactor.allocator.oom")
    src/mem/segment_provider.cpp      — FAULT_INJECT("hpactor.allocator.segment.mmap_fail")
    src/mem/slab_cache.cpp            — FAULT_INJECT("hpactor.allocator.slab_cache.refill_fail")
    src/sched/scheduler.cpp           — 12 FAULT_INJECT sites
    src/sched/timing_wheel.cpp        — FAULT_INJECT sites in schedule/advance/cancel
    src/actor/actor_system.cpp        — FAULT_INJECT sites in spawn/deliver
    src/actor/event_based_actor.cpp   — FAULT_INJECT sites in receive/become/on_exit
    src/actor/lifecycle_actor.cpp     — FAULT_INJECT site in transition()
    src/actor/actor_context.cpp       — FAULT_INJECT sites in send/reply/schedule/stop
    src/supervision/supervision.cpp   — FAULT_INJECT sites in restart/decide
    src/net/gossip_membership.cpp     — FAULT_INJECT sites in async_udp_send/handle_packet
    src/net/connection_pool.cpp       — FAULT_INJECT sites in send/flush/reconnect
    src/net/tcp_transport.cpp         — existing sites expanded with delay/corrupt
    src/net/wireframe_connection.cpp  — FAULT_INJECT sites in handle_read/flush
    src/net/acceptor.cpp              — FAULT_INJECT sites in listen/handle_read
    src/net/registrar_client.cpp      — FAULT_INJECT sites in heartbeat/register
    src/net/actor_location_cache.cpp  — FAULT_INJECT sites in get/put/evict
    src/rpc/rpc_channel.cpp           — FAULT_INJECT sites in send/response/retry
    src/tracing/trace_manager.cpp     — FAULT_INJECT sites in span/export
    src/tracing/trace_exporter.cpp    — FAULT_INJECT site in export_batch
    src/metrics/metrics_aggregator.cpp — FAULT_INJECT sites in on_event/drain
    src/cli/cli_actor.cpp             — FAULT_INJECT sites in run_once/execute_tokens
    src/cli/lexer.cpp                 — FAULT_INJECT site in tokenize
    src/config/toml_parser.cpp        — FAULT_INJECT site in parse()

tests/unit/fault/
    test_fault_controller.cpp     — Controller load/enable/check/per-thread (expanded)
    test_fault_schedule.cpp       — Schedule construction, expand_random(), sort (expanded)
    test_fault_point.cpp          — Registration, wildcard matching, enumeration (expanded)
    test_fault_macro.cpp          — (new) macro expansion patterns for each action

tests/integration/fault/
    test_fault_mailbox.cpp        — (existing, expanded) Mailbox fault injection
    test_fault_transport.cpp      — (existing, expanded) Transport send/recv
    test_fault_scheduler.cpp      — (new) Worker thread fault injection
    test_fault_allocator.cpp      — (new) OOM and corruption injection
    test_fault_actor_lifecycle.cpp — (new) Lifecycle/supervision faults
    test_fault_gossip.cpp         — (new) Gossip protocol faults
    test_fault_rpc.cpp            — (new) RPC channel faults
    test_fault_tracing.cpp        — (new) Tracing subsystem faults
    test_fault_seed_replay.cpp    — (existing, expanded) Per-thread seed replay
    test_fault_chaos_scenario.cpp — (new) Multi-subsystem chaos tests

tests/system/fault/
    test_fault_end_to_end.cpp     — (new) Full-system fault injection under load
```

## 7. Phased Implementation Plan

### Phase 1 — Infrastructure & Existing Unwired Points (1-2 days)

- Expand `FaultDomain` enum from 9 to 14 values
- Expand `domain_ticks_[]` array to 14
- Implement per-thread instance infrastructure (install/remove/broadcast/aggregate_snapshot)
- Add `LogCategory::kFault` and wire `FaultController::check()` to emit structured log
- Implement `expand_random()` and `FaultSchedule::sort()`
- Wire the 9 already-registered-but-unwired FAULT_INJECT sites
- Update `fault_points.cpp` to register fault points under new domains
- Expand existing unit tests for new infrastructure
- **Deliverable:** 12 wired sites (3 existing + 9 unwired), 14 domains, per-thread support, expand_random(), timeline log

### Phase 2 — Tier 1: Core Message/Resource Path (2-3 days)

- Wire ~43 FAULT_INJECT sites across Mailbox (8), Transport (15), Scheduler (12), Allocator (8)
- New test files: `test_fault_mailbox.cpp` (expanded), `test_fault_transport.cpp` (expanded), `test_fault_scheduler.cpp`, `test_fault_allocator.cpp`
- **Deliverable:** ~55 wired sites total, core path chaos testable

### Phase 3 — Tier 2: Resilience Path (2-3 days)

- Wire ~43 FAULT_INJECT sites across Actor Lifecycle (10), Supervision (8), Gossip (12), RPC (7), Discovery (6)
- New test files: `test_fault_actor_lifecycle.cpp`, `test_fault_gossip.cpp`, `test_fault_rpc.cpp`
- **Deliverable:** ~98 wired sites total, resilience path chaos testable

### Phase 4 — Tier 3: Observability Path (1-2 days)

- Wire ~31 FAULT_INJECT sites across Tracing (10), Metrics (7), CLI (8), Config (6)
- New test files: `test_fault_tracing.cpp`
- New system test: `test_fault_chaos_scenario.cpp` (multi-subsystem chaos)
- **Deliverable:** ~117 wired sites total, full-system chaos testable

## 8. Acceptance Criteria

- [ ] 5 new `FaultDomain` enumerators (14 total), with tick arrays and snapshot updated
- [ ] Per-thread `FaultController` instances with install/remove/broadcast/aggregate
- [ ] 9 existing registered-but-unwired fault points wired with FAULT_INJECT macros
- [ ] `expand_random()` helper with deterministic PRNG expansion and schedule sorting
- [ ] Fault timeline structured log emission on each fire, test-assertable via `MemoryLogSink`
- [ ] ~117 FAULT_INJECT sites across all 13 subsystems
- [ ] Each of the 5 fault actions (Fail, Drop, Delay, Corrupt, Panic) testable at representative sites
- [ ] Seed replay determinism preserved with per-thread instances
- [ ] CLI `/fault status threads` shows per-thread breakdown
- [ ] All existing tests pass with `ENABLE_FAULT_INJECTION=ON`
- [ ] All existing tests pass with `ENABLE_FAULT_INJECTION=OFF`
- [ ] New chaos scenario test: network partition with packet loss → system converges after faults stop
- [ ] `FAULT_INJECT` macro overhead remains zero when `ENABLE_FAULT_INJECTION=OFF`

## 9. Dependencies

- Existing `SchedulerTestDriver` pattern for deterministic test stepping.
- Existing `MpscRingBuffer` / `MetricsActor` / `MetricsRegistry` for fault metrics.
- Existing structured logging for fault timeline.
- Existing `CommandNode` trie for CLI fault commands.
- No new third-party dependencies.

## 10. Out of Scope (for TST-002 follow-up)

- Multi-process chaos harness (`ClusterHarness`, `NetworkProxy`).
- Soak test runner with memory/latency trend tracking.
- Fuzz test corpus management.
- Protocol/config compatibility matrix.
- Performance regression benchmarks.
