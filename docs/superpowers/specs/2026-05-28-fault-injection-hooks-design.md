# Deterministic Fault Injection Hooks Design

## 1. Overview

Add a deterministic fault injection framework to HPActor enabling controlled
failure simulation across all subsystems. Faults are injected via named hook
points with pre-computed schedules, making every failure reproducible from a
saved seed.

### 1.1 Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Integration model | Runtime opt-in, disabled by default | Allows staging/canary testing without recompilation; predictable branch when disabled |
| Determinism model | Pre-computed fault schedule | Strongest reproducibility — no framework-internal RNG |
| Hook naming | Hierarchical dot-separated paths with wildcards | Enables scoped enable/disable (`hpactor.transport.*`) |
| Schedule API | C++ programmatic API | Natural fit for unit tests, matches SchedulerTestDriver pattern |
| Tick model | Per-domain counters | Each subsystem advances independently; avoids coupling |
| Fault actions | Fail, Drop, Delay, Corrupt, Panic | Covers all failure modes needed for chaos testing |
| Hook mechanism | Per-subsystem fault points with global registry trie | Minimal intrusion (1-line macro), supports discovery and enumeration |

### 1.2 Goals

1. Inject controlled failures into mailbox, transport, scheduler, allocator,
   storage, timer, gossip, and config paths.
2. Reproduce any failing test from a saved seed and schedule checksum.
3. Enumerate all registered fault points from CLI for operator visibility.
4. Keep disabled-overhead to a single predictable branch per injection site.

### 1.3 Non-Goals

- Running fault injection in production builds without explicit opt-in.
- Replacing existing delivery-semantics, DLQ, or supervision tests.
- Network proxy or multi-process chaos harness (that's TST-002).

## 2. Core Components

### 2.1 FaultPoint

A named injection site registered at startup. Each `FaultPoint` carries:

- `path` — hierarchical dot-separated name (e.g., `"hpactor.mailbox.enqueue.fail"`)
- `domain` — the `FaultDomain` enum value
- `description` — human-readable string for CLI listing

FaultPoints self-register into a global static trie via file-scope static
registrar objects (same pattern as `TomlSystemParserRegistration<T>`). The
constructor appends to the trie during static initialization, before `main()`.
The trie is immutable at runtime once populated.

### 2.2 FaultDomain

Each subsystem gets its own tick counter:

```
FaultDomain       Tick source                     Example paths
────────────────────────────────────────────────────────────────
kMailbox          enqueue() / dequeue() call       hpactor.mailbox.enqueue.fail
kTransport        send() / recv() call             hpactor.transport.send.drop
kScheduler        run_one_ready() completion       hpactor.scheduler.worker.pause
kAllocator        allocate() call                  hpactor.allocator.oom
kStorage          read / write / flush call        hpactor.storage.write.fail
kTimer            timer fire / cancel              hpactor.timer.clock.skew
kGossip           gossip packet send / recv        hpactor.gossip.packet.loss
kConfig           config parse / reload            hpactor.config.reload.fail
```

### 2.3 FaultAction

Five actions a fault can trigger:

| Action | Behavior | Return semantics |
|--------|----------|------------------|
| `Fail` | Return a predefined failure result | `EnqueueResult::failure()`, `error::Code`, `nullptr`, etc. |
| `Drop` | Silently discard — return success | Returns success/true but performs no work |
| `Delay` | Pause caller for N domain ticks | Blocks or yields until delay counter depletes |
| `Corrupt` | Modify in-flight data before pass-through | Returns success, but data is altered |
| `Panic` | `std::abort()` or `SIGKILL` | Does not return |

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

**Probability expansion helper** (convenience, not framework logic):

```cpp
// Test author uses a seeded RNG to pre-expand random choices into schedule
schedule.expand_random("hpactor.transport.send.drop",
    probability = 0.05, max_ticks = 1000, rng);
// Internally calls schedule.at_domain_tick(...) for each chosen tick
```

### 2.5 FaultController

Central controller owned by `ActorSystem`. Holds the active schedule, the
enabled/disabled flag, the active scope pattern, and per-domain tick counters.

Key methods:

```cpp
class FaultController {
public:
    // Load a schedule (replaces any existing)
    void load(const FaultSchedule& schedule);
    void clear();

    // Enable/disable fault injection globally or by scope pattern
    void enable(std::string_view scope_pattern);   // e.g., "hpactor.transport.*"
    void disable(std::string_view scope_pattern);
    bool is_enabled() const;

    // Called by FAULT_INJECT macro at each injection site
    // Returns true if a fault is scheduled for this (domain, tick, path, target)
    bool check(std::string_view path, FaultDomain domain,
               std::optional<ActorId> target = std::nullopt);

    // Seed for replay (stored, not used internally)
    void set_replay_seed(uint64_t seed);
    uint64_t replay_seed() const;

    // Snapshot for metrics/CLI
    FaultControllerSnapshot snapshot() const;
};
```

**Thread safety:** The fault controller is not internally synchronized. It is
designed to be called from the same thread as the scheduler (cooperative actors)
or from a single test thread. Multi-threaded fault injection (e.g., from
background I/O threads) requires the caller to serialize access, or the fault
points on those paths use thread-local controller instances.

**Thread-local instance pointer:** A `thread_local` pointer in the controller
allows the `FAULT_INJECT` macro to access it without indirection through
ActorSystem:

```cpp
static thread_local FaultController* tls_instance = nullptr;
```

### 2.6 FAULT_INJECT Macro

The injection site macro expands to a predictable cold branch:

```cpp
#define FAULT_INJECT(path) \
    if (auto* _fc = ::hpactor::fault::FaultController::tls_instance(); \
        HPACTOR_UNLIKELY(_fc != nullptr && _fc->check(path)))
```

When the fault controller is disabled (`tls_instance` is null), the branch is a
single predictable pointer comparison. The compiler hoists the check and the
failure-handling code stays cold.

**Usage pattern at injection sites:**

```cpp
EnqueueResult MPSCActorMailbox::enqueue(T* node) {
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        return EnqueueResult::failure(FailureReason::kMailboxFull);
    }
    // ... normal fast path
}

ssize_t TcpTransport::send(span<const uint8_t> data) {
    FAULT_INJECT("hpactor.transport.send.drop") {
        return static_cast<ssize_t>(data.size());  // lie: claim success
    }
    FAULT_INJECT("hpactor.transport.send.delay") {
        _fc->stall(FaultDomain::kTransport, /*delay_ticks=*/3);
        // fall through to normal send after delay
    }
    // ... normal fast path
}
```

### 2.7 FaultPoint Registry (Global Trie)

A compile-time-constructed trie of all registered fault points. Each `FaultPoint`
definition registers its path into the trie:

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

The trie supports:
- Exact path lookup for `check()`
- Prefix walk for wildcard `enable("hpactor.transport.*")`
- Full enumeration for CLI `/fault list`

The trie is a static data structure populated via `constexpr` definitions in
each source file. No dynamic registration at runtime.

## 3. Initial Fault Point Catalog

Twelve initial fault points covering the subsystems listed in the chaos testing
design doc:

```
hpactor.mailbox.enqueue.fail           — mailbox admission failure
hpactor.mailbox.dequeue.drop           — silent message discard on dequeue
hpactor.allocator.oom                  — allocator out-of-memory
hpactor.actor.handler.delay            — actor handler delay (N ticks)
hpactor.scheduler.worker.pause         — scheduler worker pause (N ticks)
hpactor.scheduler.worker.panic         — scheduler worker crash
hpactor.transport.send.drop            — transport send drop
hpactor.transport.send.delay           — transport send delay
hpactor.transport.recv.drop            — transport receive drop
hpactor.transport.recv.corrupt         — transport receive corruption
hpactor.transport.connection.reset     — connection reset
hpactor.gossip.packet.loss             — gossip packet loss
```

Additional points are added as each subsystem integrates fault injection.

## 4. Test UX

### 4.1 Deterministic Fault Test

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

    auto actor = system.spawn<TestActor>();
    auto result = system.send(actor->address(), test_message);
    // Fault fired — result reflects mailbox full
    EXPECT_EQ(result.status(), DeliveryStatus::kFailed);
    EXPECT_EQ(result.failure_reason(), FailureReason::kMailboxFull);
}
```

### 4.2 Chaos Scenario Test (with seed replay)

```cpp
TEST(ChaosScenario, PartitionDuringRPCLoad) {
    constexpr uint64_t kSeed = 0xDEADBEEF;
    std::mt19937 rng(kSeed);

    // Build deterministic schedule from seed
    FaultSchedule schedule;
    schedule
        .expand_random("hpactor.transport.send.drop", 0.20, 500, rng)
        .expand_random("hpactor.transport.recv.drop", 0.20, 500, rng)
        .expand_random("hpactor.gossip.packet.loss", 0.30, 100, rng);

    ActorSystem system(/*...*/);
    system.fault_controller().load(schedule);
    system.fault_controller().enable("*");
    system.fault_controller().set_replay_seed(kSeed);

    // Run scenario...
    // On failure: seed 0xDEADBEEF is printed.
    // Reproduction: same seed → same schedule → same failure.
}
```

### 4.3 Integration with SchedulerTestDriver

```cpp
SchedulerTestDriver driver(system);
FaultController& fc = system.fault_controller();
fc.load(schedule);
fc.enable("*");

// Each driver.run_one() advances the kScheduler domain tick.
// Mailbox/transport/allocator domain ticks advance independently
// as the corresponding operations are called by actors.
for (int i = 0; i < 100; i++) {
    driver.run_one();
    // Faults fire deterministically at their scheduled domain ticks
}
```

## 5. Observability

### 5.1 Metrics

```
hpactor_fault_injections_total{path, action, target}  — counter, incremented per fire
hpactor_fault_schedule_entry_count                     — gauge, entries in loaded schedule
hpactor_fault_controller_enabled                       — gauge, 0 or 1
```

Integrates with existing `MpscRingBuffer` → `MetricsActor` → Prometheus path.

### 5.2 CLI Commands

Guarded by `ENABLE_CLI`:

```
/fault status                  — enabled state, scope, schedule size, replay seed
/fault list                    — enumerate all registered fault points (trie walk)
/fault enable <scope>          — enable faults matching scope pattern
/fault disable <scope>         — disable faults matching scope pattern
/fault load <schedule-file>    — load a TOML schedule file (for admin-driven chaos)
/fault clear                   — unload schedule, disable all
/fault seed [<value>]          — display or set the replay seed
```

### 5.3 Fault Timeline Log

Each fired fault emits a structured log entry:

```json
{
    "event": "fault_inject",
    "tick": 42,
    "domain": "kTransport",
    "path": "hpactor.transport.send.drop",
    "action": "Drop",
    "target": null,
    "schedule_index": 3,
    "replay_seed": 3735928559
}
```

In tests, a `MemoryLogSink` captures entries for assertion:

```cpp
REQUIRE(log_sink.contains("hpactor.mailbox.enqueue.fail"));
REQUIRE(log_sink.timeline_ordered({
    "hpactor.mailbox.enqueue.fail",
    "hpactor.transport.send.drop"
}));
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
    fault_controller.hpp          — FaultController class
    fault_schedule.hpp            — FaultSchedule, FaultScheduleEntry, FaultAction
    fault_point.hpp               — FaultPoint, FaultDomain, fault point registration
    fault_macros.hpp              — FAULT_INJECT macro

src/fault/
    fault_controller.cpp          — FaultController implementation
    fault_schedule.cpp            — FaultSchedule builder implementation
    fault_point_registry.cpp      — Global trie, FaultPoint self-registration

tests/unit/fault/
    test_fault_controller.cpp     — Controller load/enable/check/disarm
    test_fault_schedule.cpp       — Schedule construction, expansion, iteration
    test_fault_point.cpp          — Trie registration, wildcard matching, enumeration

tests/integration/fault/
    test_fault_mailbox.cpp        — Mailbox enqueue/dequeue fault injection
    test_fault_transport.cpp      — Transport send/recv drop, delay, corrupt
    test_fault_allocator.cpp      — Allocator OOM injection
    test_fault_scheduler.cpp      — Scheduler pause/panic injection
    test_fault_seed_replay.cpp    — Same seed → same schedule → same failure
```

## 7. Acceptance Criteria

- [ ] Fault injection hooks exist for mailbox, transport, scheduler, allocator
  (and storage, gossip, timer, config as stretch).
- [ ] `FAULT_INJECT` macro compiles to a predictable cold branch when disabled.
- [ ] Tests can inject each of the five fault actions (Fail, Drop, Delay,
  Corrupt, Panic).
- [ ] A failing test can be reproduced from the saved seed and schedule checksum.
- [ ] CLI `/fault list` enumerates all registered fault points.
- [ ] Metrics `hpactor_fault_injections_total` fires on each injected fault.
- [ ] Fault timeline log captures fired faults in test-assertable form.
- [ ] `ENABLE_FAULT_INJECTION=OFF` eliminates all runtime overhead.

## 8. Dependencies

- Existing `SchedulerTestDriver` pattern for deterministic test stepping.
- Existing `MpscRingBuffer` / `MetricsActor` / `MetricsRegistry` for fault
  metrics.
- Existing structured logging for fault timeline.
- Existing `CommandNode` trie for CLI fault commands.
- No new third-party dependencies.

## 9. Out of Scope (for TST-002 follow-up)

- Multi-process chaos harness (`ClusterHarness`, `NetworkProxy`).
- Soak test runner with memory/latency trend tracking.
- Fuzz test corpus management.
- Protocol/config compatibility matrix.
- Performance regression benchmarks.
