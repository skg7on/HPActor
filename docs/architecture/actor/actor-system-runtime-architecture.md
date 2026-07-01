# ActorSystem Runtime Architecture — Component Graph and Lifecycle

**Date:** 2026-07-01
**Status:** Implemented (Phases 0–8 complete)
**Supersedes:** Pre-refactor monolithic `ActorSystem` ownership model

## 1. Executive Summary

`ActorSystem` is HPActor's stable user-facing runtime facade. It preserves all
existing public APIs while delegating subsystem ownership, policy, and lifecycle
orchestration to a graph of focused runtime components behind a PImpl boundary.

**Key Architecture Decisions:**

- **Facade + Component Graph.** `ActorSystem` is a thin public facade over
  `ActorSystem::Impl`, which owns the runtime component graph. No subsystem
  policy or resource ownership lives in the facade.
- **Composition over inheritance.** Each runtime component owns one cohesive
  subsystem. Mixins are not used to distribute shared mutable state.
- **Typed dependency injection.** Components receive concrete references or
  narrow function-pointer ports at construction. There is no DI container,
  service locator, or runtime `get<T>()` lookup.
- **Non-owning lifecycle coordinator.** `RuntimeCoordinator` mediates startup,
  readiness, drain, and shutdown ordering without owning subsystem state.
- **Immutable pre-start blueprint.** `RuntimeBlueprint` is validated before any
  threads, listeners, actors, or daemonization side effects.
- **No RTTI, no exceptions, no virtual dispatch on hot paths.** Virtual
  interfaces are confined to cross-library control-plane boundaries.

---

## 2. Target Component Graph

```
                        ┌─────────────────────────┐
                        │    ActorSystem facade    │
                        │  (public API, thin       │
                        │   forwards only)         │
                        └────────────┬────────────┘
                                     │ std::unique_ptr<Impl>
                        ┌────────────▼────────────┐
                        │   ActorSystem::Impl     │
                        │  (composition root,     │
                        │   owns component graph) │
                        └────────────┬────────────┘
                                     │
        ┌────────┬────────┬──────────┼──────────┬─────────┬──────────┐
        ▼        ▼        ▼          ▼          ▼         ▼          ▼
   ┌────────┐┌───────┐┌────────┐┌────────┐┌────────┐┌────────┐┌──────────┐
   │ Actor  ││Messag-││Network ││ Stream ││Observ- ││ICluster││ Runtime  │
   │Runtime ││ingRun-││Runtime ││Runtime ││ability ││Runtime ││Blueprint │
   │        ││time   ││        ││        ││Runtime ││        ││(immutable)│
   └───┬────┘└───┬───┘└───┬────┘└───┬────┘└───┬────┘└───┬────┘└─────┬────┘
       │         │        │         │         │        │          │
       │         │        │    ┌────▼────┐    │        │    ┌─────▼──────┐
       │         │        │    │Inbound  │    │        │    │ Runtime    │
       │         │        │    │Frame    │    │        │    │ Coordinator│
       │         │        │    │Router   │    │        │    │ (non-owning│
       │         │        │    └─────────┘    │        │    │  mediator) │
       │         │        │                   │        │    └────────────┘
```

`RuntimeCoordinator` holds non-owning references to every started component and
sequences lifecycle transitions. No component owns or calls back into the entire
`ActorSystem`.

---

## 3. Component Ownership and Contracts

### 3.1 `ActorSystem` — Stable Facade

**File:** `include/hpactor/actor/actor_system.hpp`, `src/actor/actor_system.cpp`

**Owns:** Only `std::unique_ptr<Impl> impl_`.

**Responsibilities:**
- Preserve existing public signatures and default behavior.
- Validate facade-level preconditions.
- Delegate to exactly one owning component per operation.
- Expose narrow non-owning capability views (`actors()`, `messaging()`,
  `network()`, `operations()`).
- Expose compatibility accessors (deprecated where safe snapshots exist).

**Non-responsibilities:**
- Owning subsystem fields directly.
- Starting threads or registering transport callbacks.
- Parsing TOML or selecting actor scheduling policy.
- Decoding wire frames or holding stream registries.

### 3.2 `ActorRuntime`

**File:** `src/runtime/actor_runtime.hpp/.cpp`

**Owns:**
- `ActorDirectory` — sole source of truth for actor ids, entries, and names.
- `ActorSpawner` — sole publisher of complete `ActorDirectoryEntry`.
- `ActorTypeRegistry` — registered spawnable actor types.
- `PassivationManager` — actor passivation lifecycle.
- System pseudo-actor identity and fixed typed handles (metrics, CLI,
  receptionist, HTTP gateway, spawn receiver actors).

**Responsibilities:**
- Local actor identity and lifetime.
- Actor lookup, enumeration, name registration, and removal.
- Unified adoption of constructed actors via `ActorSpawner::adopt(SpawnSpec)`.
- Atomic directory publication (id + optional name under one mutex).
- Spawn admission gate: close before directory publication, release-open after
  activation/lifecycle initialization completes.

**`ActorRuntime` does not** deliver ordinary messages, own a transport, parse
topology, or manage telemetry backends.

#### Spawn Adoption Pipeline

All actor creation (template `spawn<T>()`, TOML-configured, reserved system,
remote-spawn factories) converges on one path:

```cpp
struct SpawnSpec {
    std::string_view type_name;
    std::optional<std::string_view> registered_name;
    mailbox::MailboxConfig mailbox;
    sched::DispatchPolicy dispatch_policy;
    sched::DispatchHints dispatch_hints;
    std::optional<config::QuarantinePolicy> quarantine;
    std::optional<ActorId> reserved_id;          // system range 0xFFFF0000-0xFFFFFFFF
    std::optional<ActorType> actor_type_override; // for established system addresses
    SpawnOrigin origin;                           // observability only
};
```

Adoption state machine:

```
Validating → Preparing → Publishing → Activating → Dispatchable → Observed
     │           │            │            │
     └───────────┴────────────┴────────────┘
                      │
                   Failed (entry erased, admission reopened)
```

1. Validate actor mode, spec, reserved id, quarantine compatibility.
2. Close spawn admission gate on the actor.
3. Allocate/validate id, assign address, copy type name.
4. Create mailbox and context; bind context (RTTI-free capability check).
5. Inject scheduler, mailbox, metrics, logger references.
6. Apply quarantine configuration.
7. Atomically publish `ActorDirectoryEntry` + optional name under one mutex.
8. Invoke `activate_after_spawn()` → existing `on_activate()` contract.
9. Transition optional lifecycle mixin to active.
10. Open spawn admission with release ordering.
11. Register dedicated dispatch or notify cooperative readiness exactly once.
12. Emit spawn log and metric with origin and type.

**No actor becomes runnable before activation completes.** The spawn admission
gate is an atomic flag on `AbstractActor`, default-open for source compatibility.
Only `ActorSpawner` closes and reopens it. `ActorReadyGate` and scheduler
workers check the gate with acquire semantics.

#### `ActorSpawner`

```cpp
class ActorSpawner final {
public:
    struct Dependencies {
        ActorSystem& facade;
        EndPoint endpoint;
        ActorDirectory& directory;
        sched::IScheduler& scheduler;
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics;
        log::Logger* logger;
    };

    result<Actor> adopt(std::shared_ptr<AbstractActor> actor,
                        const SpawnSpec& spec) noexcept;
};
```

The spawner stores only stable non-owning references. All owners outlive it.

### 3.3 `MessagingRuntime`

**File:** `src/runtime/messaging_runtime.hpp/.cpp`

**Owns:**
- `DeadLetterQueue` — stable object identity; never replaced while borrowers
  hold its address. `reconfigure()` mutates policy in place.
- `DeliveryPipeline` — full admission policy: TTL, dedup, circuit breaker,
  backpressure, DLQ routing.
- `LocalDeliveryEngine` — fast local delivery with explicit restricted semantics.
- `BackpressureCoordinator` — local and remote backpressure signal coordination.
- `DedupCache` — receiver-side `(source_node, source_actor, message_id)`
  duplicate suppression.
- `msg::OutboundDeliveryTracker` — reliable receipt and retry state.
- `mailbox::OutboundTracker` — bounded per-destination compatibility API.

**Responsibilities:**
- Full local delivery admission and result mapping.
- Fast internal delivery (`FastDeliveryReason::StreamProtocol`,
  `FastDeliveryReason::CompatibilityExplicit`).
- DLQ, deduplication, TTL, circuit-breaker, ACK/NACK, and pressure integration.
- Stable instrumentation sinks for delivery.

**Hot-path constraints:**
- No virtual dispatch, allocation, or service lookup per message.
- Concrete references to `ActorDirectory`, mailbox types, and stable telemetry
  sinks.
- Preserves mailbox reservation, release, single-consumer, ready-gate, and
  lost-wakeup contracts.

**Convergence:** Ordinary local actor messages and decoded remote actor
messages both enter through `MessagingRuntime::try_deliver()` — one full-policy
path.

**Network control ports** use fixed function-pointer/context pairs (no
`std::function`, no facade-capturing lambdas):

- `ReliableAckPort` — `void (*)(void* ctx, const ReliableAck&)` for reliable
  ACK/NACK emission.
- `BackpressureWirePort` — `void (*)(void* ctx, const BackpressureSignal&)` for
  remote backpressure signal emission.

These ports are bound to stable `NetworkRuntimeState` in
`src/runtime/messaging_network_ports.hpp`.

### 3.4 `NetworkRuntime`

**File:** `include/hpactor/runtime/network_runtime.hpp`,
`src/runtime/network_runtime.cpp`

**Owns:**
- `TcpTransport` and its authoritative `EventLoop` — **one loop**, not two.
  Discovery, HTTP, cache purge, reliable retry timers, and connection I/O all
  bind to `transport->loop()`.
- Network thread — drives the single event loop.
- `IServiceDiscovery` / `UdpRegistrar` — pluggable discovery backends.
- `ActorLocationCache` — TTL cache for ActorId → EndPoint resolution.
- Cache and retry maintenance timers.
- `RpcChannel` — async RPC with at-least-once delivery.
- `HttpClient` — HTTP client for external gateway integration.
- Remote-spawn client and receiver network integration.

**Construction is side-effect-free.** `start()` uses 9-stage startup with
reverse rollback on failure. `stop()` is idempotent and callback-quiescent.

**Port types** (fixed-size, non-owning, function-pointer + `void*` context):

| Port | Signature | Purpose |
|------|-----------|---------|
| `NodeEventSink` | `void (*)(void*, const NodeEvent&)` | Node join/leave/death events |
| `OutboundRetryPort` | `void (*)(void*, ActorId, TypedMessage&&)` | Reliable retry emission |
| `RemoteSpawnPort` | `void (*)(void*, const SpawnRequest&)` | Remote spawn requests |
| `InboundFrameSinkPort` | `void (*)(void*, WireFrame&&)` | Frame ingress → router |
| `NetworkTelemetryPort` | `void (*)(void*, const MetricEvent&)` | Network metrics |

Networking-disabled systems: `NetworkRuntime` pointer is absent. Compatibility
accessors return `nullptr`. No dummy event loop, transport, or discovery object
exists.

### 3.5 `InboundFrameRouter`

**File:** `src/net/inbound_frame_router.hpp/.cpp`

A stateless demultiplexer that classifies every valid HPActor wire envelope by
oneof type:

```
WireFrame
  ├── Data (ordinary actor data) ──────► MessagingRuntime::try_deliver()
  ├── Data (RPC response) ─────────────► RpcChannel
  ├── Ack / Nack (reliable control) ───► typed reliable handlers
  ├── Batch ───────────────────────────► bounded per-entry full-policy delivery
  ├── Stream (open/data/ack/close/err)─► StreamRuntime
  └── Invalid/unsupported ─────────────► typed dispatch result + metrics/log/DLQ
```

**Design rules:**
- Executes on the network-loop thread.
- Handler table assembled once; uses concrete references/function pointers.
- No runtime service lookup or per-frame allocation.
- Malformed frames return explicit `FrameDispatchResult`; `NetworkRuntime`
  decides transport policy (drop, close, quarantine, report peer).
- Reliable flag disambiguation: dual-bit (`AckRequested`+`AckResponse`) →
  legacy ACK; `AckResponse`-only → legacy NACK; `AckRequested`-only → ordinary
  data metadata.
- Batch delivery: 1024-entry limit, partial aggregation on overflow.

### 3.6 `StreamRuntime`

**File:** `src/actor/stream_runtime.hpp/.cpp`

**Owns:**
- Stream id allocation (`std::atomic<uint64_t>` counter).
- Peer-qualified bounded session map (`StreamKey{EndPoint, uint64_t}`, max
  4096 active streams).
- Sender and receiver actor registries.
- Stream open/data/ack/close/error protocol handlers.
- Stream-specific metrics and `StreamRuntimeSnapshot` for bounded CLI/admin
  visibility.

**Two-phase open:**
1. Reserve an Opening slot in the bounded registry.
2. Spawn internal stream actors.
3. Commit to Active on success; roll back the Opening slot on failure.

**Concurrency contract:**
- Network callbacks may look up or remove stream entries.
- Actor or external threads may open and register streams.
- A dedicated mutex protects only registry structure and id-to-actor mapping.
- Callers copy the required `ActorId`, then release the lock before spawning,
  delivery, transport calls, logging, metrics, or callbacks.
- Stream payload delivery continues through the existing explicitly documented
  fast path (`FastDeliveryReason::StreamProtocol`).

### 3.7 `ObservabilityRuntime`

**File:** `include/hpactor/runtime/observability_runtime.hpp`,
`src/runtime/observability_runtime.cpp`

**Owns:**
- Metrics ring buffer (`MpscRingBuffer<MetricEvent>`) and aggregator.
- `LogManager` and logger sinks.
- `TraceManager` and trace exporters.
- `FaultController` (deterministic fault injection).

**Responsibilities:**
- Start telemetry consumers before producers publish events.
- Keep sink object identity stable for the runtime lifetime.
- Stop producers before destroying sinks.
- Apply only configuration classified as `LiveReloadable`.
- Flush best effort during shutdown without blocking cooperative workers.

**Stable telemetry ports** (`MetricsSinkPort`, `LogSinkPort`, `TraceSinkPort`)
are passed to producers (scheduler, messaging, network, actors) at construction.
No producer discovers telemetry through the facade or a service locator.

`MetricsActor` is actor-owned (belongs to `ActorRuntime`); `ObservabilityRuntime`
owns the ring buffer, config, and exporter infrastructure only.

### 3.8 `IClusterRuntime` — Cross-Library Control-Plane Adapter

**File:** `include/hpactor/cluster/cluster_runtime.hpp`

A typed interface replacing the pre-refactor `unique_ptr<void>` + cleanup
function + `static_cast` pattern:

```cpp
class IClusterRuntime {
public:
    virtual ~IClusterRuntime() = default;
    virtual result<void> start() noexcept = 0;
    virtual result<void> begin_leave() noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual ClusterStatusSnapshot snapshot() const = 0;
};
```

This virtual boundary is acceptable because it is a **control-plane** boundary,
not a per-message hot path. Core code owns `std::unique_ptr<IClusterRuntime>`
and never stores `void*`, cleanup function pointers, or downcasts.

### 3.9 `RuntimeBlueprint` — Immutable Validated Startup Input

**File:** `include/hpactor/runtime/runtime_blueprint.hpp`,
`src/runtime/runtime_blueprint_builder.hpp/.cpp`

Separates configuration into two distinct outputs:

1. **`RuntimeBlueprint`** — effective, validated subsystem configuration required
   before runtime start. Immutable after construction. FNV-1a fingerprint for
   reload diff detection.
2. **`ActorTopologyPlan`** — validated actor definitions and dependency order,
   executed after core runtime startup.

**Pipeline:**

```
Config/TOML → Subsystem-owned parsers → Cross-subsystem validation
  ├── Immutable RuntimeBlueprint → RuntimeBuilder → component graph
  └── ActorTopologyPlan → TopologyBootstrapper.apply() after runtime is ready
```

**Reload classification:** Every config field has a `ReloadClass`:

| Class | Behavior |
|-------|----------|
| `Live` | Safe to apply to a running system through owning subsystem |
| `RestartRequired` | Requires component or full restart |
| `Immutable` | Cannot change after construction |

`load_topology()` remains source-compatible. Running `load_topology()` validates
and rejects `Immutable`/`RestartRequired` changes atomically before any mutation
or actor spawn.

### 3.10 `RuntimeBuilder` — Composition Root

**File:** `include/hpactor/runtime/runtime_builder.hpp`,
`src/runtime/runtime_builder.hpp/.cpp`

Constructs the complete stopped component graph from a validated
`RuntimeBlueprint`. No threads, listeners, or actors are started during
construction. Produces a stopped `ActorSystem` ready for
`RuntimeCoordinator::start()`.

### 3.11 `RuntimeCoordinator` — Non-Owning Lifecycle Orchestrator

**File:** `include/hpactor/runtime/runtime_coordinator.hpp`,
`src/runtime/runtime_coordinator.hpp/.cpp`

A non-owning mediator that sequences lifecycle transitions. Holds references to
already-constructed components. Contains no subsystem policy.

**Startup sequence:**
1. Validate `Constructed` state.
2. Prepare process mode (daemonization before threads).
3. Start observability consumers.
4. Install fault-injection integration.
5. Start scheduler workers.
6. Start network loop, discovery, transport, ingress (non-ready).
7. Start optional cluster adapter.
8. Adopt required system actors.
9. Apply actor topology plan.
10. Publish readiness only after required components and actors report ready.

On failure: stop every successfully started phase in reverse order; return the
original structured failure plus rollback findings.

**Shutdown sequence:**
1. Atomic readiness → false; enter `DrainingIngress`.
2. Stop external ingress (HTTP, remote-spawn, ordinary).
3. Snapshot actors; execute per-actor drain policy.
4. Ask cluster adapter to leave.
5. Stop network timers, transport, discovery, event loop, network thread.
6. Stop scheduler workers after actor execution is quiescent.
7. Flush traces, logs, metrics, DLQ (best effort).
8. Remove fault hooks; release process resources.
9. Enter `Stopped` or `ForcedStop` with structured reason.

**Coordinator rules:**
- `start()` and `stop()` are idempotent.
- Only the control thread executes phase transitions.
- Data-plane callbacks never call coordinator phase methods directly.
- No component lock is held while starting, stopping, draining, or invoking
  another component.
- Destruction delegates to idempotent stop (no second independent shutdown
  sequence).

---

## 4. Data and Control Flows

### 4.1 Local Message Delivery

```
ActorContext / ActorRef / facade
  → MessagingRuntime::try_deliver()
    → ActorDirectory lookup
    → DeliveryPipeline: dedup → circuit breaker → TTL → backpressure → admission
    → MPSCActorMailbox enqueue
    → ActorReadyGate / scheduler notification
    → result + DLQ/metrics/backpressure as required
```

### 4.2 Remote Message Ingress

```
Transport callback on network loop
  → InboundFrameRouter::dispatch()
    → oneof classification
    → ordinary data: decode TypedMessage → MessagingRuntime::try_deliver()
    → same local mailbox path
```

Remote metadata and trace context are decoded before the call to messaging and
are not reconstructed downstream.

### 4.3 Actor Spawning

```
template/config/system factory
  → construct AbstractActor with required arguments
  → ActorSystem::spawn<T>() / spawn_configured() facade adapter
  → ActorSpawner::adopt(SpawnSpec)
  → unified directory/mailbox/context/lifecycle/scheduler/telemetry sequence
```

### 4.4 Runtime Configuration

```
parse → validate → classify diff → plan
  ├── startup: build immutable blueprint before side effects
  └── reload:  reject immutable/restart-required changes atomically
               apply live changes through owning subsystem
               emit audit/log/metric outcome
```

---

## 5. Ownership and Lifetime Contract

| Resource | Sole owner | Borrowers | Destruction requirement |
|----------|------------|-----------|------------------------|
| Actor entries and names | `ActorRuntime`/`ActorDirectory` | Scheduler, messaging, CLI snapshots | Stop execution before destroying entries |
| Scheduler workers | Scheduler component | Actor runtime, mailboxes | Join workers before actor/telemetry teardown |
| DLQ, dedup, delivery trackers | `MessagingRuntime` | Delivery pipeline, CLI snapshots, scheduler expiry | Stable identity until delivery producers stop |
| Event loop/thread/transport | `NetworkRuntime` | Frame router, RPC, stream, backpressure | Stop ingress and join thread before handlers die |
| Stream maps | `StreamRuntime` | Frame router, stream APIs | Quiesce network callback, then clear registry |
| Metrics/log/trace managers | `ObservabilityRuntime` | All producers | Producers stop before sink destruction |
| Cluster implementation | Cluster adapter owner | Coordinator, operations queries | Leave/stop before network and identity teardown |
| Runtime configuration | Immutable `RuntimeBlueprint` | All components | Lives for the whole runtime |

Raw pointers may be used on hot paths only when this table guarantees a stable
owner and shutdown ordering proves the pointee outlives every borrower.
Replacing an owned object in place is **prohibited** while borrowers retain
its address.

---

## 6. Dependency Injection and Inversion Rules

1. A component constructor receives only dependencies needed for its steady
   state.
2. Optional runtime links use nullable narrow ports or explicit optional
   adapters, not access to `ActorSystem`.
3. Setter injection is limited to lifecycle transitions that are genuinely
   cyclic; the preferred design eliminates such cycles in `RuntimeBuilder`.
4. Concrete references inside one library where ownership and compilation
   dependencies are stable.
5. Virtual interfaces only at true replaceable or cross-library boundaries
   (`IScheduler`, `IServiceDiscovery`, `IClusterRuntime`).
6. **No dependency lookup on message, mailbox, scheduler, or network hot paths.**

---

## 7. Error and Failure Semantics

- Construction and startup helpers return `result<T>`; they do not throw.
- A failed component start includes component, phase, canonical `FailureReason`,
  and underlying detail.
- Startup rollback is best effort but observable. Rollback failure never hides
  the initiating error.
- Actor adoption returns an error without publishing a partial directory entry,
  or removes it before returning if failure occurs after publication.
- Invalid or unsupported frames produce a typed dispatch result and bounded
  observability; they never invoke actor code directly.
- Reload rejects unsupported changes before partial application.
- Shutdown timeouts enter `ForcedStop` with structured reason.
- Facade `void` compatibility methods preserve existing behavior but delegate to
  result-returning internals and record failures through observability policy.

---

## 8. Preferred Startup API

```cpp
// Result-returning factory (recommended production API):
static result<std::unique_ptr<ActorSystem>>
ActorSystem::create(Config config) noexcept;

static result<std::unique_ptr<ActorSystem>>
ActorSystem::create(config, topology_path) noexcept;

// Legacy constructor (source-compatible, delegates internally):
ActorSystem::ActorSystem(const Config& config);
```

The result-returning factories parse and validate the complete startup blueprint
**before** any side effects (no threads, listeners, actors, or daemonization
during validation). The legacy constructor uses the same internal builder;
detected startup failures leave the object stopped and not ready.

---

## 9. Operations and Observability Contract

Preserved surfaces:
- Existing actor, delivery, backpressure, scheduler, transport, DLQ, and
  shutdown metrics.
- Existing structured logs and trace propagation.
- CLI/admin actor listing, lookup, DLQ inspection, timer stats, health,
  readiness, and shutdown status.
- Fault-injection sites and deterministic failure testing.

Component-level additions:
- Runtime lifecycle phase and duration.
- Component startup and rollback failure counts.
- Config validation/reload rejection counts by `ReloadClass`.
- Inbound frame dispatch failures by payload category.
- Active stream registry size and rejected/unknown stream frame counts.

**Readiness contract:**
- `false` until required system actors, scheduler, and configured ingress are
  operational.
- `false` before ingress drain begins.
- Liveness is independent of readiness.

CLI/admin views consume **snapshots** from owning components. They must not
reach through the facade into component containers or hold internal locks while
formatting output.

---

## 10. Public API and Compatibility Strategy

### Preserved throughout migration
- `ActorSystem(const Config&)` constructor.
- `spawn<T>()` and configured spawn behavior.
- `ActorContext::send()`, `ActorRef::send()`, delivery result APIs.
- `load_topology()` signature.
- Shutdown, readiness, timer, metrics, logging, tracing, network, registry, and
  cluster-facing accessors.
- Actor address, message, TypeTag, and wire compatibility.

### Added as preferred APIs
- Result-returning `ActorSystem::create()` factory.
- Narrow non-owning capability views: `actors()`, `messaging()`, `network()`,
  `operations()`.

### Deprecated (source-compatible, future removal)
- Unsafe raw subsystem-pointer accessors (marked `[[deprecated]]` where safe
  snapshots exist).
- Legacy `ActorRegistry` compatibility view (backed by `ActorDirectory`).

---

## 11. Source and Build Layout

```
include/hpactor/actor/actor_system.hpp          # Public facade (pointer-only private state)
include/hpactor/runtime/
  network_runtime.hpp                            # NetworkRuntime public header
  observability_runtime.hpp                      # ObservabilityRuntime public header
  runtime_blueprint.hpp                          # RuntimeBlueprint + RuntimeBlueprintBuilder
  runtime_coordinator.hpp                        # RuntimeCoordinator
  runtime_builder.hpp                            # RuntimeBuilder
  runtime_startup.hpp                            # Startup stage registration
include/hpactor/cluster/cluster_runtime.hpp      # IClusterRuntime interface
include/hpactor/net/
  frame_dispatch_result.hpp                      # FrameDispatchCode/FrameDispatchResult
  inbound_frame_sink.hpp                         # InboundFrameSink port type
  network_snapshot.hpp                           # NetworkSnapshot for bounded CLI/admin visibility
include/hpactor/actor/stream_snapshot.hpp        # StreamRuntimeSnapshot
include/hpactor/config/reload_report.hpp         # ReloadClass, ReloadReport

src/runtime/
  actor_system_impl.hpp/.cpp                     # ActorSystem::Impl (private to hpactor_lib)
  actor_runtime.hpp/.cpp                         # ActorRuntime
  actor_spawner.hpp/.cpp                         # ActorSpawner
  spawn_spec.hpp                                 # SpawnSpec
  messaging_runtime.hpp/.cpp                     # MessagingRuntime
  messaging_network_ports.hpp                    # ReliableAckPort, BackpressureWirePort
  runtime_blueprint_builder.hpp/.cpp             # RuntimeBlueprintBuilder
  runtime_builder.hpp/.cpp                       # RuntimeBuilder
  runtime_coordinator.hpp/.cpp                   # RuntimeCoordinator
  runtime_startup.hpp/.cpp                       # Startup stage wiring
  observability_runtime.cpp                      # ObservabilityRuntime implementation
  network_runtime.cpp                            # NetworkRuntime implementation

src/net/
  inbound_frame_router.hpp/.cpp                  # InboundFrameRouter

src/actor/
  stream_runtime.hpp/.cpp                        # StreamRuntime
```

Runtime-internal headers are private to `hpactor_lib`. They must never be added
to the public include tree or installed. Tests may receive a private
`${CMAKE_SOURCE_DIR}/src` include path.

---

## 12. Architecture Fitness Rules

Enforced by CTest architecture checks:

1. `ActorSystem` facade header contains only `std::unique_ptr<Impl>` as runtime
   state. No subsystem fields, config copies, or raw manager pointers in the
   public header private section.
2. No `ActorSystem*` or `Impl*` capture in messaging, frame routing, stream
   runtime, network runtime, or lifecycle production code.
3. No late-setter methods on components after construction (`set_metrics()`,
   `set_transport()`, etc. removed).
4. No RTTI (`dynamic_cast`, `typeid`) or exception flow in runtime files.
5. No `std::function` in `NetworkRuntime`, `MessagingRuntime`, or
   `RuntimeCoordinator` hot paths.
6. `ActorDirectoryEntry` construction only in `ActorSpawner` and tests.
7. `ActorDirectory::publish()` called only by `ActorSpawner`.
8. No scheduler production member stores `ActorSystem&`.
9. One event loop only (`TcpTransport::loop()`); no second `network_loop_`.
10. No `unique_ptr<void>` cluster ownership or `static_cast` downcasts in core.

---

## 13. Relationship to Other Architecture Docs

| Document | Relationship |
|----------|-------------|
| `actors-data-structure-design.md` | Actor type hierarchy, dispatch policy, lifecycle — now owned by `ActorRuntime` |
| `mailbox-management-backpressure-design.md` | Mailbox design, backpressure, overflow — now owned by `MessagingRuntime` |
| `streaming-message-protocol-core-concept.md` | Stream protocol design — now owned by `StreamRuntime`, routed by `InboundFrameRouter` |
| `actor-routing-design.md` | Router subsystem — routees are actors in `ActorRuntime` |
| `actor-metrics-design.md` | Metrics — ring buffer and exporters owned by `ObservabilityRuntime` |
| `actor-logging-core-concept.md` | Structured logging — sinks owned by `ObservabilityRuntime` |
| `distributed-tracing-design.md` | Distributed tracing — manager owned by `ObservabilityRuntime` |
| `cli-interactive-design.md` | CLI — consumes component snapshots, not facade internals |
| `actor-concurrency-and-lockfree-mailbox-rules.md` | Concurrency rules — normative; unchanged by this refactor |
| `user-defined-actor-programming-model.md` | Programming model — unchanged; spawn now routes through `ActorSpawner` |
