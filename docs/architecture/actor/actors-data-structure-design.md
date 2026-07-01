# HPActor — Actor Core Concept and Architecture

**Runtime ownership:** Actor identity, lookup, spawning, and lifecycle are owned
by `ActorRuntime`. See
[actor-system-runtime-architecture.md](actor-system-runtime-architecture.md) for
the full component graph and ownership contracts.

## 1. Executive Summary

HPActor actors follow an **event-based** programming model with **turn-based
concurrency**, supporting both **statically typed** and **dynamically typed**
messaging. The design is inspired by CAF (C++ Actor Framework).

**Key Design Decisions:**

- **Event-based actors with cooperative M:N work-stealing.** Most actors share a
  pool of worker threads; each activation processes one message to completion.
- **Dispatch policy as an attribute, not a class.** What an actor *is* (its type,
  behavior, supervision) is orthogonal to *how* it is scheduled. The
  `DispatchPolicy` tells the scheduler which execution model to use.
- **Pinned dispatch for specialized workloads.** Daemon, I/O, and compute-heavy
  actors get dedicated threads or pools while remaining in the supervision tree
  and participating in normal message-passing.
- **Explicit lifecycle with passivation.** Ten lifecycle states from spawn
  through active, drain, stop, failure, recovery, quarantine, and passivation.
  Idle actors can release memory while retaining identity and optional durable
  state.
- **Hierarchical supervision.** OneForOne and AllForOne restart strategies with
  configurable retry windows.
- **Unified actor references.** `ActorRef` is a variant over local actor
  pointers and remote proxies, making location transparent to senders.
- **Unified spawn adoption.** All actor creation (template `spawn<T>()`,
  TOML-configured, reserved system, remote-spawn factories) converges through
  `ActorSpawner::adopt(SpawnSpec)` — one transactional pipeline for directory
  publication, context binding, lifecycle transition, and dispatch registration.

---

## 2. Dispatch Policy — Separating Kind from Execution

The `DispatchPolicy` bridges actor identity and scheduling:

| Policy | Execution model | Use case |
|--------|----------------|----------|
| `Cooperative` | M:N work-stealing pool (default) | Short, non-blocking message handlers |
| `DedicatedThread` | Own OS thread, may block on I/O | Daemons, DPDK pollers, event loops |
| `DedicatedPool` | Private thread pool (1+ threads) | ML inference, crypto, image processing |

A `DaemonActor` *is* an `EventBasedActor` — it has a mailbox, a behavior, and a
supervisor. The only difference is that `dispatch_policy()` returns
`DedicatedThread`, so the scheduler never queues it on cooperative workers.

**Benefits of policy-as-attribute:**

| Concern | Where it lives |
|---------|---------------|
| Actor identity, messaging, supervision | Actor class hierarchy |
| How the actor is scheduled | `DispatchPolicy` + `DispatchHints` |
| Lifecycle (start/drain/stop/passivate) | `LifecycleActor` state machine |

---

## 3. Actor Type Hierarchy

```
                          ┌──────────────────────────────┐
                          │        AbstractActor          │
                          │  id(), system(), link_to()    │
                          │  receive(TypedMessage&) = 0   │
                          │  dispatch_policy() const = 0  │
                          └──────────────┬───────────────┘
                                         │
                     ┌───────────────────┼───────────────────┐
                     ▼                   ▼                   ▼
              ┌────────────┐    ┌──────────────┐    ┌──────────────┐
              │ LocalActor  │    │ (future)     │    │ (future)     │
              │ context()   │    │ RemoteActor  │    │ VirtualActor │
              └──────┬──────┘    └──────────────┘    └──────────────┘
                     │
       ┌─────────────┼────────────────────────┐
       ▼             ▼              ▼         ▼
┌────────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐
│EventBased  │ │Blocking  │ │Typed     │ │ScopedActor   │
│Actor       │ │Actor     │ │Actor<>   │ │(non-actor    │
│(cooperative│ │(threaded)│ │(static)  │ │ contexts)    │
└─────┬──────┘ └──────────┘ └──────────┘ └──────────────┘
      │
      ├── StatefulActor<T>        (explicit state member)
      ├── SupervisorActor          (manages child supervision)
      ├── SelfSupervisingActor     (supervision + parent in one)
      │     └── PoolRouter         (routee pool + supervision)
      ├── GroupRouter              (external routee routing by service key)
      └── SpawnReceiver            (handles remote spawn requests)

Specialization via DispatchPolicy (NOT inheritance):

  EventBasedActor + DedicatedThread
      ├── DaemonActor        (long-running event loop)
      ├── PollingActor        (DPDK-style busy-poll)
      ├── ExternalMsgGatewayActor  (protocol ingress)
      │     └── HTTPGatewayActor    (HTTP server)
      └── CliActor            (interactive CLI)

  EventBasedActor + DedicatedPool
      └── DenseComputingActor (ML inference, crypto)
```

All of these are **actors** — they have addresses, mailboxes, behaviors, and
supervisors. The only thing that changes is where their execution runs.

---

## 4. Fundamental Types

| Type | Purpose |
|------|---------|
| `ActorId` | Opaque `uint64_t` unique identifier |
| `ActorType` | `uint32_t` — identifies the "class" of an actor |
| `incarnation_type` | `uint64_t` — incremented on restart, detects stale references |
| `MessageId` | `uint64_t` — unique per-message identifier |
| `ActorAddress` | `(NodeId, ActorType, ActorId, incarnation)` — network-routable address |
| `CommunicationEndpoint` | `variant<Ipv4Endpoint, Ipv6Endpoint>` — wire-level endpoint |
| `error` | Error code + message, no exceptions in hot path |
| `result<T>` | Value or error, used for all fallible operations |
| `Clock` | Wraps `steady_clock` for timestamps and scheduling |
| `TraceContext` | `(trace_id, span_id, flags)` — W3C-compatible |
| `StreamBuffer` | Owned byte buffer for serialization |

---

## 5. Base Actor Classes

### AbstractActor

Interface root. Provides identity (`id()`, `address()`), linking (`link_to()`,
`unlink_from()`), monitoring (`monitor()`, `demonitor()`), child spawning, and
the pure virtual `receive()`. Exposes `dispatch_policy()` and
`dispatch_hints()` so the scheduler knows how to execute this actor.
Serialization hooks (`serialize_state()`, `mailbox_snapshot()`) support CLI
introspection and hibernation.

### LocalActor

Adds `ActorContext*` access — the per-actor execution context used for sending
messages, spawning children, and scheduling timers. All locally-executed actors
inherit from this.

### EventBasedActor

The default actor type. Uses a `Behavior` (set of message handlers) and
`become()` for dynamic handler replacement. Each activation dequeues one
message, dispatches through the behavior, and returns. This is the base for
`StatefulActor<T>`, `SupervisorActor`, and all dispatch-policy specializations.

### TypedEventBasedActor\<Signatures...\>

Compile-time type-safe actor. Handlers are checked against the declared
signature set. No runtime type dispatch.

### BlockingActor / ScopedActor

Thread-based actors with blocking `receive()`. `BlockingActor` gets a dedicated
thread. `ScopedActor` is for non-actor contexts like `main()`.

---

## 6. Special Actors with Pinned Dispatch

Systems handle heterogeneous workloads. Pinned dispatch ensures blocking or
compute-heavy actors never starve the cooperative pool:

| Workload | Challenge | Solution |
|----------|-----------|----------|
| Heavy I/O (DB, files, sockets) | Blocks worker threads | `DedicatedThread` |
| DPDK packet polling | Busy-poll, 100% CPU on a core | `DedicatedThread` + CPU affinity |
| Dense computation (ML, crypto) | Long-running, starves pool | `DedicatedPool` |
| Protocol gateways (HTTP, gRPC) | Own event loop (epoll/kqueue) | `DedicatedThread` |

**DaemonActor** — runs a `run_once()` loop on a dedicated thread. Subclassed by
`PollingActor` (DPDK), `ExternalMsgGatewayActor` (protocol ingress),
`HTTPGatewayActor`, and `CliActor`.

**DenseComputingActor** — handlers dispatched to a private thread pool. The
actor still participates in message-passing; only execution is isolated.

---

### 6.1 Actor Routers

Routers distribute messages across a set of routees using a pluggable routing
strategy. Two router types cover the primary use cases:

**PoolRouter** extends `SelfSupervisingActor`. It spawns a pool of child actors
(routees), supervises them (restart on failure, quarantine on excessive
failures), and forwards messages to the routee selected by the current
`IRoutingLogic`. Supports broadcast, resize, and runtime routing logic swap.

**GroupRouter** extends `EventBasedActor`. It routes to externally-registered
actor references discovered by a service key string. Routees are not children
and receive no supervision from the router. Supports add/remove/set routees,
broadcast, and runtime routing logic swap.

**Routing strategies** (all implement `IRoutingLogic`):

| Strategy | Selection | State |
|----------|-----------|-------|
| `RoundRobinLogic` | Atomic counter, sequential | `atomic<uint64_t>` |
| `RandomLogic` | xorshift64 PRNG, CAS-based | `atomic<uint64_t>` |
| `ConsistentHashingLogic` | Hash ring, 128 vnodes, O(log n) | Sorted ring vector |
| `SmallestMailboxLogic` | Lowest `MboxSnapshot::depth` | Stateless |

**No-RTTI design:** `IRoutingLogic::on_routees_changed()` is a virtual hook
called when routees are added/removed. `ConsistentHashingLogic` overrides it to
rebuild the hash ring; other strategies leave it as a no-op. This avoids
`dynamic_cast` (prohibited by `-fno-rtti`).

See [Actor Routing Design](actor-routing-design.md) for the full architecture,
API, message flow, supervision integration, and usage examples.

---

## 7. ActorContext and ActorSystem

### ActorContext

Per-actor execution context. The actor's interface to the runtime:

- **Messaging**: `send()`, `reply()`, `reply_with_error()`, `try_send()`
- **Scheduling**: `schedule(delay, msg)`, `cancel_schedule(handle)`
- **Lifecycle**: `passivate()` — request self-passivation
- **Supervision**: `spawn()`, `add_child()`, `remove_child()`
- **Linking**: `link_to()`, `unlink_from()`, `monitor()`, `demonitor()`

### ActorSystem

The actor environment. Owns the scheduler, actor registry, clock, metrics ring
buffer, log manager, trace manager, DLQ, durable state store, and fault
controller. Provides `spawn()` for system-level actors and
`load_topology("config.toml")` for declarative bootstrap.

---

## 8. Behavior and Message Handling

An actor's **Behavior** is a set of message handlers. When a message arrives,
the behavior matches it by type and invokes the handler. `become()` replaces the
current behavior dynamically — this is how actors model state machines.

**TypedBehavior** provides compile-time checking: the handler set must match the
actor's declared signature list.

Messages are delivered as `TypedMessage` (protobuf `TypeTag` + serialized
payload). System messages (`DownMsg`, `LinkMsg`, `ExitMsg`, `InspectStateRequest`,
etc.) use reserved TypeTags below 64. User messages start at TypeTag 64.

---

## 9. Supervision

Hierarchical fault tolerance. When a child fails, the supervisor decides:
**Restart**, **Stop**, or **Escalate**.

| Strategy | Behavior |
|----------|----------|
| `OneForOne` | Only the failed child restarts |
| `AllForOne` | All children restart when one fails |

`SupervisorActor` manages children with a configurable policy (max restarts,
sliding time window). `SelfSupervisingActor` combines supervisor and parent in
one class — it can spawn children and handle their failures directly.

Supervision is **opaque to dispatch policy**. A `DaemonActor` child failure is
handled identically to a cooperative child failure. The supervisor never knows
or cares which thread the child ran on.

---

## 10. Actor Lifecycle and Passivation

### Lifecycle States

Ten states with validated CAS transitions through a constexpr state table:

```
kStarting ──► kActive ──► kPassivating ──► kPassivated ──► kRecovering ──► kActive
   │             │              │                │                │
   ▼             ▼              ▼                ▼                ▼
kFailed       kDraining      kFailed          kStopped         kFailed
   │             │              │
   ▼             ▼              ▼
kStopped      kStopping    kQuarantined
                 │
                 ▼
              kStopped
```

| State | Accepts user msgs | Purpose |
|-------|-------------------|---------|
| `kStarting` | No | Initial spawn |
| `kActive` | **Yes** | Normal operation |
| `kDraining` | No | Draining before stop/passivation |
| `kStopping` | No | Final cleanup |
| `kStopped` | No | Terminal |
| `kFailed` | No | Awaiting supervisor decision |
| `kRecovering` | No | Restoring durable state |
| `kQuarantined` | No | Isolated after repeated failure |
| `kPassivating` | No | Draining + snapshotting |
| `kPassivated` | No | Memory freed, route stub alive |

### LifecycleActor

A mixin providing CAS-based `transition(to)` with virtual hooks invoked after
each transition (`on_start()`, `on_drain()`, `on_fail()`, `on_recover()`,
`on_passivating()`, `on_passivated()`, etc.). The constexpr `StateDef` table
validates every transition at compile time and runtime.

### Passivation

Passivation releases an idle actor's memory while preserving its identity and
optional durable state. The actor can later be reactivated when a message
arrives.

**Triggers:** idle timeout, self-request via `context()->passivate()`, memory
pressure (LRU selection), CLI command.

**Protocol:** `Active → Passivating` (drain mailbox) → persist snapshot (if
durable) → `Passivated` (release memory, install route stub). Reactivation
reverses this: message arrives at route stub → `Recovering` (restore state) →
`Active`.

**Durable actors** implement `IDurableActor` (snapshot/restore/migrate) and
persist through a `DurableStateStore` backend (`InMemoryStateStore` for tests,
`FileStateStore` for local durability). **Memory-only actors** implement
`Hibernatable` and store in `HibernationRegistry`.

**Route handling** uses the `IActorRoute` interface. `LocalActiveRoute` wraps a
live actor. `LocalPassivatedRoute` buffers incoming messages in a bounded queue
and triggers lazy reactivation. `ShardOwnedRoute` (future) forwards to a shard
owner.

See [ACT-008 Design Spec](../../superpowers/specs/2026-06-06-act-008-actor-passivation-design.md)
for the full passivation protocol, failure semantics, fault injection, and
observability design.

---

## 11. Actor References and Addresses

- **`ActorAddress`** — `(NodeId, ActorType, ActorId, incarnation)`. Network-routable, serializable.
- **`Actor`** — `shared_ptr<AbstractActor>`, opaque local handle.
- **`ActorProxy`** — reference to a possibly-remote actor via `Transport`.
- **`ActorRef`** — `variant<Actor, ActorProxy>`, the unified reference type. Senders use `ActorRef` without knowing whether the target is local or remote.

---

## 12. Mailbox Integration

Messages enter the system as `TypedMessage` (TypeTag + protobuf payload). System
messages (TypeTags 1–63) include `DownMsg`, `ExitMsg`, `LinkMsg`, `UnlinkMsg`,
and introspection messages. User messages start at TypeTag 64.

The mailbox subsystem (see `mailbox-management-backpressure-design.md`) handles
bounded admission, multi-lane priority routing, overflow policies, backpressure
signals, dead-letter queue, and delivery semantics. The actor layer interacts
with mailboxes only through `ActorContext::send()` / `try_send()` and the
`receive()` entry point.

---

## 13. Key File Layout

```
include/hpactor/
├── actor/                  # Actor base classes, lifecycle, routes, passivation
│   ├── abstract_actor.hpp
│   ├── local_actor.hpp
│   ├── event_based_actor.hpp
│   ├── typed_actor.hpp
│   ├── stateful_actor.hpp
│   ├── lifecycle_state.hpp, lifecycle_actor.hpp
│   ├── passivation_config.hpp
│   ├── durable_actor.hpp, durable_state_store.hpp
│   ├── actor_route.hpp
│   ├── daemon_actor.hpp, polling_actor.hpp, dense_computing_actor.hpp
│   └── routing/            # Actor routers — workload distribution
│       ├── routing_logic.hpp
│       ├── pool_router.hpp
│       └── group_router.hpp
├── core/                   # ActorSystem, ActorContext
├── ref/                    # ActorAddress, ActorRef, ActorProxy
├── mailbox/                # Mailbox, DLQ, overflow handlers, backpressure
├── sched/                  # Scheduler, worker threads, dispatch policy
├── supervision/            # Supervision strategies
├── mem/                    # Slab allocator, hibernation registry
├── metrics/                # Actor metrics + OpenMetrics
├── log/                    # Structured logging
├── tracing/                # Distributed tracing (W3C TraceContext)
├── cli/                    # Interactive CLI
├── fault/                  # Deterministic fault injection
├── config/                 # TOML topology + subsystem parsers
└── types/                  # Core types, failure reason, failure envelope
```

---

## 14. References

- [ACT-008 Passivation Design Spec](../../superpowers/specs/2026-06-06-act-008-actor-passivation-design.md)
- [Actor Routing Design](actor-routing-design.md)
- [Actor Concurrency and Lock-Free Mailbox Rules](actor-concurrency-and-lockfree-mailbox-rules.md)
- [Mailbox Management and Backpressure Design](mailbox-management-backpressure-design.md)
- [Distributed Actor System Architecture](../system-architecture-and-key-concept-high-level-design.md)
- [CAF Actor Types](https://github.com/actor-framework/actor-framework)
