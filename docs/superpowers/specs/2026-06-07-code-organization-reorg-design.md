# Code Organization Reorganization — Design Spec

**Date:** 2026-06-07
**Status:** Approved
**Topic:** Reorganize `include/hpactor/{actor,mailbox,sched}/` headers to align with architecture design docs

## 1. Motivation

The architecture design documents in `docs/architecture/` define clear subsystem boundaries:

- **`scheduling-architecture-design.md`** specifies `::timer` and `::memory` (coroutine frames) as **separate modules** with no upward dependencies — pure utilities consumed by `::sched`. Currently `timing_wheel.hpp`, `calendar_queue.hpp`, and all coroutine infrastructure live inside `sched/`.
- **`actors-data-structure-design.md`** identifies lifecycle (startup, shutdown, passivation, drain, quarantine) as a **distinct subsystem** from core actor types. Currently 37 files are flat in `actor/` with no subdirectory separation.
- **`mailbox-management-backpressure-design.md`** places backpressure coordination, delivery engines, and memory pressure monitoring as **mailbox admission concerns**, not actor concerns. Currently these files live in `actor/`.

The current flat organization obscures subsystem boundaries, makes it harder for new contributors to understand the architecture, and drifts from the documented design.

## 2. Design

### 2.1 New Directory: `include/hpactor/timer/`

**Rationale:** Architecture doc states timer is a pure utility module with no upward dependencies.

| File | Old Location | New Location |
|------|-------------|-------------|
| `timing_wheel.hpp` | `include/hpactor/sched/timing_wheel.hpp` | `include/hpactor/timer/timing_wheel.hpp` |
| `calendar_queue.hpp` | `include/hpactor/sched/calendar_queue.hpp` | `include/hpactor/timer/calendar_queue.hpp` |

### 2.2 New Directory: `include/hpactor/coroutine/`

**Rationale:** Architecture doc places coroutine frame pool under `::memory` as a utility. Extracted as `coroutine/` since it encompasses more than memory (tasks, awaiters, coroutine handles) and serves the scheduler's cooperative execution model.

| File | Old Location | New Location |
|------|-------------|-------------|
| `coroutine_task.hpp` | `include/hpactor/sched/coroutine_task.hpp` | `include/hpactor/coroutine/coroutine_task.hpp` |
| `coroutine_awaiters.hpp` | `include/hpactor/sched/coroutine_awaiters.hpp` | `include/hpactor/coroutine/coroutine_awaiters.hpp` |
| `coroutine_frame_pool.hpp` | `include/hpactor/sched/coroutine_frame_pool.hpp` | `include/hpactor/coroutine/coroutine_frame_pool.hpp` |
| `actor_coroutine.hpp` | `include/hpactor/sched/actor_coroutine.hpp` | `include/hpactor/coroutine/actor_coroutine.hpp` |
| `yield_awaiter.hpp` | `include/hpactor/sched/yield_awaiter.hpp` | `include/hpactor/coroutine/yield_awaiter.hpp` |

### 2.3 New Directory: `include/hpactor/actor/lifecycle/`

**Rationale:** Architecture doc identifies lifecycle as a distinct subsystem. 12 files covering lifecycle state machine, graceful shutdown, passivation, drain, quarantine, and circuit breaker reliability patterns are grouped together.

| File | Old Location | New Location |
|------|-------------|-------------|
| `lifecycle_actor.hpp` | `include/hpactor/actor/lifecycle_actor.hpp` | `include/hpactor/actor/lifecycle/lifecycle_actor.hpp` |
| `lifecycle_state.hpp` | `include/hpactor/actor/lifecycle_state.hpp` | `include/hpactor/actor/lifecycle/lifecycle_state.hpp` |
| `shutdown_phase.hpp` | `include/hpactor/actor/shutdown_phase.hpp` | `include/hpactor/actor/lifecycle/shutdown_phase.hpp` |
| `shutdown_coordinator.hpp` | `include/hpactor/actor/shutdown_coordinator.hpp` | `include/hpactor/actor/lifecycle/shutdown_coordinator.hpp` |
| `passivation_manager.hpp` | `include/hpactor/actor/passivation_manager.hpp` | `include/hpactor/actor/lifecycle/passivation_manager.hpp` |
| `passivation_config.hpp` | `include/hpactor/actor/passivation_config.hpp` | `include/hpactor/actor/lifecycle/passivation_config.hpp` |
| `drain_policy.hpp` | `include/hpactor/actor/drain_policy.hpp` | `include/hpactor/actor/lifecycle/drain_policy.hpp` |
| `drain_config.hpp` | `include/hpactor/actor/drain_config.hpp` | `include/hpactor/actor/lifecycle/drain_config.hpp` |
| `quarantine_policy.hpp` | `include/hpactor/actor/quarantine_policy.hpp` | `include/hpactor/actor/lifecycle/quarantine_policy.hpp` |
| `quarantine_reason.hpp` | `include/hpactor/actor/quarantine_reason.hpp` | `include/hpactor/actor/lifecycle/quarantine_reason.hpp` |
| `circuit_breaker.hpp` | `include/hpactor/actor/circuit_breaker.hpp` | `include/hpactor/actor/lifecycle/circuit_breaker.hpp` |
| `failure_rate_tracker.hpp` | `include/hpactor/actor/failure_rate_tracker.hpp` | `include/hpactor/actor/lifecycle/failure_rate_tracker.hpp` |

### 2.4 Move to `include/hpactor/mailbox/`

**Rationale:** Architecture doc places backpressure, delivery, and pressure monitoring as mailbox admission concerns. The `mailbox-management-backpressure-design.md` explicitly describes the mailbox subsystem boundary at `ActorSystem::try_deliver_local` → `MailboxAdmission` → `BoundedActorMailbox`.

| File | Old Location | New Location |
|------|-------------|-------------|
| `backpressure_coordinator.hpp` | `include/hpactor/actor/backpressure_coordinator.hpp` | `include/hpactor/mailbox/backpressure_coordinator.hpp` |
| `local_delivery_engine.hpp` | `include/hpactor/actor/local_delivery_engine.hpp` | `include/hpactor/mailbox/local_delivery_engine.hpp` |
| `memory_pressure_monitor.hpp` | `include/hpactor/actor/memory_pressure_monitor.hpp` | `include/hpactor/mailbox/memory_pressure_monitor.hpp` |

### 2.5 Move to `include/hpactor/actor/durable/`

**Rationale:** `durable/` subdirectory already exists with `durable_state_store.hpp`. `durable_actor.hpp` belongs alongside it.

| File | Old Location | New Location |
|------|-------------|-------------|
| `durable_actor.hpp` | `include/hpactor/actor/durable_actor.hpp` | `include/hpactor/actor/durable/durable_actor.hpp` |

### 2.6 `include/hpactor/sched/` After Reorganization

11 files remain (down from 18):

```
include/hpactor/sched/
├── scheduler.hpp                 # IScheduler + HybridScheduler
├── scheduler_interfaces.hpp      # Narrow interface segregation
├── worker_thread.hpp             # WorkerThread
├── work_queue.hpp                # ChaselevDeque, MultiPriorityWorkQueue
├── edf_queue.hpp                 # EDFPriorityQueue
├── a2ws.hpp                      # A2WSCoordinator, LoadSnapshot
├── work_placement_scheduler.hpp  # Work placement strategy
├── actor_execution_engine.hpp    # Coroutine execution engine
├── actor_ready_gate.hpp          # Ready-gate interface
├── dedicated_thread_pool.hpp     # Dedicated thread pool
└── dispatch_policy.hpp           # DispatchPolicy enum
```

### 2.7 `include/hpactor/actor/` After Reorganization

21 files remain + 2 subdirectories (down from 37):

```
include/hpactor/actor/
├── abstract_actor.hpp            # Interface base
├── local_actor.hpp               # ActorContext access
├── event_based_actor.hpp         # Cooperative, behavior-based
├── stateful_actor.hpp            # Explicit state<T>
├── proto_stateful_actor.hpp      # Protobuf-native state
├── typed_actor.hpp               # Static typing
├── blocking_actor.hpp            # Thread-per-actor
├── scoped_actor.hpp              # Main / non-actor contexts
├── daemon_actor.hpp              # DedicatedThread
├── polling_actor.hpp             # CPU-affinity polling
├── dense_computing_actor.hpp     # DedicatedPool
├── external_msg_gateway.hpp      # Protocol ingress
├── http_gateway_actor.hpp        # HTTP ingress
├── spawn_receiver.hpp            # System actor
├── actor_fwd.hpp                 # Forward declarations
├── actor_context.hpp             # Per-actor execution context
├── actor_state.hpp               # Actor state machine (Idle/Ready/Running)
├── actor_route.hpp               # Actor routing
├── actor_directory.hpp           # Actor directory
├── ask_manager.hpp               # Ask/request tracking
├── durable/                      # Durable state subdirectory
│   ├── durable_state_store.hpp
│   └── durable_actor.hpp
└── lifecycle/                    # Lifecycle subdirectory
    ├── lifecycle_actor.hpp
    ├── lifecycle_state.hpp
    ├── shutdown_phase.hpp
    ├── shutdown_coordinator.hpp
    ├── passivation_manager.hpp
    ├── passivation_config.hpp
    ├── drain_policy.hpp
    ├── drain_config.hpp
    ├── quarantine_policy.hpp
    ├── quarantine_reason.hpp
    ├── circuit_breaker.hpp
    └── failure_rate_tracker.hpp
```

## 3. Source File Moves

| Current | New |
|---------|-----|
| `src/sched/timing_wheel.cpp` | `src/timer/timing_wheel.cpp` |
| `src/adt/calendar_queue.cpp` | `src/timer/calendar_queue.cpp` |
| `src/sched/coroutine_frame_pool.cpp` | `src/coroutine/coroutine_frame_pool.cpp` |
| `src/actor/backpressure_coordinator.cpp` | `src/mailbox/backpressure_coordinator.cpp` |
| `src/actor/local_delivery_engine.cpp` | `src/mailbox/local_delivery_engine.cpp` |
| `src/actor/memory_pressure_monitor.cpp` | `src/mailbox/memory_pressure_monitor.cpp` |
| `src/actor/lifecycle_actor.cpp` | `src/actor/lifecycle/lifecycle_actor.cpp` |
| `src/actor/passivation_manager.cpp` | `src/actor/lifecycle/passivation_manager.cpp` |
| `src/actor/quarantine_reason.cpp` | `src/actor/lifecycle/quarantine_reason.cpp` |

## 4. Include Path Updates

All `#include` directives referencing moved files must be updated atomically. Affected files include:

**Timer consumers:**
- `include/hpactor/sched/scheduler.hpp` → `"hpactor/timer/timing_wheel.hpp"`
- `src/timer/timing_wheel.cpp` → `"hpactor/timer/calendar_queue.hpp"`

**Coroutine consumers:**
- `include/hpactor/sched/scheduler.hpp` / `worker_thread.hpp` / coroutine headers
- `include/hpactor/actor/event_based_actor.hpp`
- `src/sched/actor_execution_engine.cpp`

**Lifecycle consumers:**
- `include/hpactor/core/actor_system.hpp`
- `include/hpactor/net/connection_pool.hpp`
- `include/hpactor/config/topology_model.hpp`
- `include/hpactor/supervision/supervision.hpp`
- `include/hpactor/actor/event_based_actor.hpp`
- `include/hpactor/actor/actor_route.hpp`
- `include/hpactor/actor/lifecycle/shutdown_coordinator.hpp`
- `src/actor/actor_system.cpp`
- `src/actor/abstract_actor.cpp`
- `src/actor/event_based_actor.cpp`
- `src/supervision/supervision.cpp`
- `src/net/endpoint_circuit_breaker.cpp`

**Delivery consumers:**
- `src/actor/actor_system.cpp`
- `include/hpactor/core/actor_system.hpp`

**Durable consumers:**
- `src/actor/lifecycle/passivation_manager.cpp`

**Test file includes** — all tests referencing moved headers must be updated.

## 5. Migration Strategy

**Atomic update, no forwarding headers.** All moves and include path updates happen in a single commit. No deprecated stubs at old locations.

## 6. CMake Changes

Add new source directories to the build:
- `src/timer/`
- `src/coroutine/`
- `src/actor/lifecycle/`

## 7. CLAUDE.md Enforcement Rule

Add to `CLAUDE.md` under architecture section:

```
### Header Placement Rules

New headers must be placed according to architecture module boundaries:

| Concern | Directory |
|---------|-----------|
| Timer types (timing wheel, calendar queue) | `include/hpactor/timer/` |
| Coroutine infrastructure (tasks, awaiters, frame pool) | `include/hpactor/coroutine/` |
| Lifecycle, shutdown, passivation, drain, quarantine, circuit breaker | `include/hpactor/actor/lifecycle/` |
| Mailbox admission, backpressure, delivery, pressure monitoring | `include/hpactor/mailbox/` |
| Scheduler internals (scheduler, workers, queues, EDF, A2WS, dispatch) | `include/hpactor/sched/` |
```

## 8. Verification

1. **Compile**: `cmake -S . -B build -GNinja && ninja -C build` — zero errors
2. **Tests**: `ctest --output-on-failure --parallel 8` — all tests pass
3. **Stale path audit**: grep for old paths returns zero results:
   - `sched/timing_wheel`, `sched/calendar_queue`, `sched/coroutine_task`, `sched/coroutine_awaiters`, `sched/coroutine_frame_pool`, `sched/yield_awaiter`, `sched/actor_coroutine`
   - `actor/lifecycle_actor.hpp` (not under `actor/lifecycle/`), `actor/shutdown_*`, `actor/passivation_*`, `actor/drain_*`, `actor/quarantine_*`, `actor/circuit_breaker.hpp`, `actor/failure_rate_tracker.hpp`
   - `actor/backpressure_coordinator.hpp`, `actor/local_delivery_engine.hpp`, `actor/memory_pressure_monitor.hpp`
   - `actor/durable_actor.hpp` (not under `actor/durable/`)

## 9. Summary

| Category | Count |
|----------|-------|
| New directories | 3 (`timer/`, `coroutine/`, `actor/lifecycle/`) |
| Headers moved | 22 |
| Source files moved | 9 |
| Include paths updated | ~35-50 across include/, src/, tests/ |
| CMake source directories added | 3 |
| Forwarding headers | 0 (atomic) |
| Files in `sched/` after | 11 (was 18) |
| Files in `actor/` after | 21 + 2 subdirs (was 37) |
