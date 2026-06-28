# ActorSystem Phase 3 MessagingRuntime Design

**Date:** 2026-06-28

**Status:** Proposed phase design

**Parent design:**
`docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`

**Prerequisites:** Phase 0 correctness stabilization, Phase 1 runtime ownership
shell, and Phase 2 `ActorRuntime`/scheduler dependency narrowing are merged and
their focused normal, ASan, and TSAN verification passes.

**Scope:** Replace the Phase 1 messaging storage group with one cohesive
`MessagingRuntime`. Move local delivery policy, dead letters, deduplication,
backpressure coordination, fast local delivery, and both outbound trackers
under that owner. Remove delivery callbacks which capture `ActorSystem`, while
leaving wire-frame demultiplexing and stream protocol state for Phase 4 and
network lifecycle ownership for Phase 5.

## 1. Summary

Phase 3 makes messaging a real subsystem rather than a set of objects that
happen to be constructed by `ActorSystem`. `MessagingRuntime` becomes the sole
owner of the dead-letter queue, receiver dedup cache, delivery pipeline, fast
local delivery engine, backpressure coordinator, reliable-delivery tracker,
and compatibility outbound tracker.

The ordinary local and decoded-remote actor-message paths converge on one
full-policy delivery entry point. The existing fast path remains separate and
explicit because stream protocol actors currently depend on its restricted
semantics. `ActorSystem` preserves every public delivery, DLQ, dedup, tracker,
and backpressure API as a source-compatible forwarding adapter.

The key dependency change is as important as the ownership move. The delivery
pipeline receives direct references to `ActorDirectory` and its sibling
messaging components. Network control output uses fixed, narrow function
pointer/context ports bound to stable Phase 1 network state. No messaging
object captures or stores `ActorSystem`, discovers services at runtime, or
requires a generic DI container.

Phase 3 does not redesign frame decoding. `ActorSystem::deliver_remote()` still
recognizes stream, ACK/NACK, backpressure, and ordinary data frames. It forwards
typed ACK/NACK events and decoded ordinary messages into `MessagingRuntime`.
Phase 4 moves that demultiplexing into `InboundFrameRouter` without another
messaging ownership change.

## 2. Current-State Evidence

The current implementation already contains useful extracted algorithms, but
their ownership and wiring remain centralized:

1. `ActorSystem` constructs `DeadLetterQueue`, `DedupCache`,
   `OutboundDeliveryTracker`, `mailbox::OutboundTracker`,
   `BackpressureCoordinator`, `DeliveryPipeline`, and `LocalDeliveryEngine`.
2. `DeliveryPipeline::Config` stores raw pointers plus `std::function`
   callbacks for actor lookup, mailbox lookup, local/remote backpressure, and
   reliable ACK/NACK emission.
3. Those callbacks are assembled in the `ActorSystem` constructor and capture
   the facade, granting delivery code access to the entire system and coupling
   callback lifetime to facade lifetime.
4. `DeliveryPipeline::set_metrics()` and
   `BackpressureCoordinator::set_metrics_ring_buffer()`/
   `set_transport()` repair constructor-order problems after construction.
5. The full-policy methods already delegate to `DeliveryPipeline`, while the
   explicitly named fast method delegates to `LocalDeliveryEngine`.
6. `ActorSystem::deliver_remote()` still parses control flags, mutates the
   reliable tracker directly, handles backpressure frames, decodes ordinary
   messages, and finally invokes local delivery.
7. The scheduler caches a DLQ dependency established in Phase 2, while topology
   reload and public accessors may also retain the same object address.
8. Two distinct outbound trackers exist. `msg::OutboundDeliveryTracker` owns
   reliable receipt/retry state; `mailbox::OutboundTracker` exposes a bounded
   per-destination compatibility API and has no demonstrated production
   convergence with the reliable tracker.

This phase consolidates ownership and dependency direction. It preserves the
existing admission algorithms unless a characterized correctness defect below
requires a narrowly tested correction.

## 3. Important Correctness Findings

These findings must be represented by characterization or regression tests
before the related ownership or wiring changes.

### 3.1 DLQ identity is a cross-component lifetime contract

The delivery pipeline, scheduler expiry path, facade accessors, CLI/admin
snapshots, and tests can retain a DLQ pointer. Replacing the DLQ during topology
reload invalidates those references and can produce use-after-free.

Required contract:

- `MessagingRuntime` constructs exactly one `DeadLetterQueue` object;
- its address is stable from messaging construction until all delivery and
  scheduler producers stop;
- reload mutates validated live configuration through `reconfigure()` and
  never replaces the object; and
- scheduler expiry and delivery rejection use that exact same queue.

Phase 0 should already stabilize this behavior. Phase 3 preserves and enforces
it through ownership and identity tests.

### 3.2 Constructor callbacks create broad authority and hidden lifetime edges

Delivery callbacks capturing `ActorSystem` can call unrelated services and are
valid only while the facade and every transitively accessed member remain
alive. The type system does not expose those requirements.

Required contract: `DeliveryPipeline` stores concrete sibling references for
actor lookup, DLQ, dedup, backpressure, and reliable tracking. It stores only a
narrow, fixed ACK output port for the network-side action. No production
callback captures `ActorSystem` or `ActorSystem::Impl`.

### 3.3 Late pointer setters are unsafe once ingress can run

The current metrics and transport setters mutate unsynchronized configuration.
If a network callback or producer runs concurrently, this is a data race even
when the pointed-to object itself is thread-safe.

Required contract:

- telemetry pointers and actor-directory references are fixed at construction;
- control-output ports point to one stable network-state context whose internal
  lifecycle owns transport availability;
- messaging does not rewrite raw dependency pointers after workers or ingress
  start; and
- the test-only backpressure sink is installed only before traffic begins, or
  is made explicitly synchronized in its own focused change.

### 3.4 Full and fast delivery have intentionally different semantics

The full pipeline applies actor lookup, circuit-breaker admission, default TTL,
deduplication, deadline checks, mailbox result mapping, DLQ policy,
observability, reliable tracking, and pressure signaling. The fast engine
performs direct local enqueue and bypasses some or all of those policies.

Silently routing ordinary ingress through the fast path would be a correctness
regression. Silently routing existing stream protocol traffic through the full
path could alter latency, metadata, deduplication, or pressure behavior.

Required contract:

- ordinary local send and decoded ordinary remote ingress use the full path;
- the fast entry point is separately named and requires an internal
  `FastDeliveryReason`;
- the public compatibility method maps to an explicit legacy reason;
- current stream call sites are characterized and remain on the fast path
  until Phase 4 owns them; and
- architecture tests prevent new ordinary ingress from using the fast engine.

### 3.5 Remote control parsing and tracker ownership are currently interleaved

`ActorSystem::deliver_remote()` decodes reliable ACK/NACK flags and updates the
tracker directly. Moving frame parsing in Phase 3 would overlap Phase 4, but
leaving tracker mutation in the facade would leave messaging policy split.

Required contract: Phase 3 leaves flag and payload interpretation in
`deliver_remote()` but forwards typed values to
`MessagingRuntime::on_reliable_ack()` or `on_reliable_nack()`. Phase 4 moves
the parsing without changing those typed handler contracts.

Backpressure frame decoding similarly remains a transitional forwarding seam
until Phase 4. No new frame parser is introduced in `MessagingRuntime`.

### 3.6 Reliable retry tracking is not equivalent to successful resend

The current timer invokes `OutboundDeliveryTracker::process_retries()`, but the
observed resend callback is incomplete. Ownership extraction must not claim
that end-to-end reliable resend is implemented merely because retry state is
processed.

Required contract: characterize and preserve current retry behavior, move timer
access behind `MessagingRuntime`, and document the missing resend integration
as a separate reliability-plane gap. Adding transport resend semantics is not
part of this extraction phase.

### 3.7 The two outbound trackers are not interchangeable

`msg::OutboundDeliveryTracker` and `mailbox::OutboundTracker` have different
state models and APIs. Merging them while moving ownership would combine
architecture refactoring with behavior redesign and could change bounds,
locking, ACK interpretation, or public accessors.

Required contract: `MessagingRuntime` owns both as separately named members,
preserves their existing accessors, and does not synchronize or translate
between them. A later design may retire or unify the compatibility tracker only
after its users and wire semantics are measured.

### 3.8 Metadata must survive facade and remote adapters unchanged

Sender, receiver, trace context, priority, deadline, delivery mode, message id,
ACK flags, and EDF scheduling flags are consumed at different pipeline stages.
An apparently mechanical forwarding change can accidentally reset a default or
move from an envelope field to a function default.

Required contract: table-driven tests compare all metadata at the mailbox/DLQ,
tracker, ACK, and metric boundaries before and after extraction. Adapters move
the original `TypedMessage` once and copy no payload solely for forwarding.

### 3.9 Backpressure callbacks must not run under mailbox or directory locks

Backpressure handling can reach an `ActorContext`, metrics ring, serialization,
and transport output. Invoking it while a mailbox reservation, directory lock,
or actor-state critical section is held risks lock inversion, re-entrancy, and
long stalls on delivery paths.

Required contract: preserve the current reservation/release sequence and emit
signals only after the mailbox operation returns. Directory lookup returns the
required actor/context handle before invoking user-visible backpressure code;
no directory lock crosses the callback.

### 3.10 Metrics and test sinks need stable publication rules

Metrics callbacks and test wire sinks are mutable today. Even if mutation
normally occurs during startup, the API does not state that temporal rule.

Required contract: production metrics are fixed before start. A test sink is
either pre-start-only with assertions/documentation or protected by a small
coordinator-local mutex. The messaging runtime itself does not gain a broad
lock.

### 3.11 Destruction order must be the reverse of dependency order

`DeliveryPipeline` refers to the DLQ, dedup cache, trackers, and backpressure
coordinator. The fast engine and coordinator refer to `ActorDirectory`.
Scheduler execution also refers to the DLQ. A member move that looks correct at
startup can still produce shutdown use-after-free.

Required contract:

- ingress and retry timers stop before messaging destruction;
- scheduler workers stop before the messaging-owned DLQ is destroyed;
- `DeliveryPipeline` and `LocalDeliveryEngine` are destroyed before the
  components they reference;
- `ActorRuntime`/its directory outlive `MessagingRuntime`; and
- lifecycle tests exercise networking-disabled, failed-start, normal stop, and
  destructor-only paths.

## 4. Goals

1. Make `MessagingRuntime` the sole owner of local delivery policy and messaging
   state.
2. Preserve all public `ActorSystem` delivery, backpressure, DLQ, dedup, and
   tracker signatures as thin source-compatible adapters.
3. Make full-policy delivery the single ordinary local and decoded-remote
   actor-message ingress.
4. Keep fast delivery explicit, restricted, and testable.
5. Replace facade-capturing delivery callbacks with concrete references and
   narrow fixed control-output ports.
6. Preserve stable DLQ, telemetry, tracker, dedup, and directory dependency
   identities.
7. Move typed ACK/NACK state transitions and retry-timer interaction behind
   the messaging owner without moving frame demultiplexing.
8. Preserve message metadata, result mapping, bounded capacity, and existing
   observability.
9. Preserve mailbox MPSC, reservation, ready-gate, actor single-consumer, and
   lost-wakeup contracts.
10. Leave a narrow, stable handoff for Phase 4 `InboundFrameRouter` and Phase 5
    `NetworkRuntime`.

## 5. Non-Goals

- Moving stream maps, stream frame handlers, or stream actors.
- Replacing `ActorSystem::deliver_remote()` with `InboundFrameRouter`.
- Owning the event loop, transport, discovery, remote spawn, RPC, or network
  timers.
- Changing wire flags, protobuf schemas, TypeTags, ACK status values, or
  backpressure serialization.
- Completing reliable resend or changing retry policy.
- Merging the two outbound tracker implementations.
- Redesigning mailbox algorithms, overflow policies, circuit breakers, or
  delivery-result values.
- Introducing a public `MessagingRuntime` API, generic service locator, DI
  container, virtual delivery interface, or mixin hierarchy.
- Removing raw public compatibility accessors in this phase.
- Moving configuration parsing into messaging; Phase 3 accepts already
  validated effective configuration.
- Adding exceptions, RTTI, or per-message allocation/indirection.

## 6. Considered Approaches

### 6.1 Wrap the existing objects but preserve callbacks and setters

Move the existing `unique_ptr`s into a `MessagingRuntimeState`-like class and
leave `DeliveryPipeline::Config` unchanged.

This changes the field location but preserves the hidden facade dependency,
late mutable wiring, and unclear ownership. It would be a storage shuffle, not
an architectural extraction. Rejected.

### 6.2 Make `ActorSystem` a mixin composition

Derive `ActorSystem` from delivery, DLQ, backpressure, actor, and network
mixins. This can shorten the facade source but distributes private state across
base classes, makes initialization/destruction order less obvious, and permits
cross-mixin protected access. It also does not solve subsystem ownership or
orchestration without ownership. Rejected.

### 6.3 Introduce virtual messaging interfaces and a DI container

Resolve actor lookup, DLQ, telemetry, transport, and trackers through abstract
interfaces or named runtime services.

This is flexible for tests but adds indirect lookup to hot delivery paths,
allows missing services at runtime, and obscures the concrete lifetime graph.
Rejected.

### 6.4 Rewrite delivery and reliability as one new pipeline

Unify local/remote delivery, the two trackers, ACK encoding, retry resend, and
backpressure in a new implementation.

This might be a future destination, but it combines ownership extraction with
protocol and behavior changes too large to characterize safely in one phase.
Rejected.

### 6.5 Cohesive component with concrete dependencies and narrow ports

Own existing algorithms in `MessagingRuntime`, replace in-process callbacks
with direct references, use fixed function-pointer/context ports only where a
network action crosses a phase boundary, and preserve facade APIs.

Selected. It gives explicit ownership and inversion of control without a
container, keeps the hot path concrete, and creates the typed seams Phase 4 and
Phase 5 need.

## 7. Target Phase 3 Architecture

```text
ActorSystem public facade
  |
  +-- local/full delivery -----------------------------+
  +-- explicit fast delivery --------------------------+
  +-- DLQ/dedup/tracker/backpressure accessors --------+
  +-- decoded remote ordinary message -----------------+
  +-- typed reliable ACK/NACK --------------------------+
                                                        v
                                              MessagingRuntime
                                              | owns
                                              +-- DeadLetterQueue
                                              +-- DedupCache
                                              +-- reliable tracker
                                              +-- compatibility tracker
                                              +-- BackpressureCoordinator
                                              +-- DeliveryPipeline
                                              +-- LocalDeliveryEngine
                                                | refs
                                                +-- ActorDirectory
                                                +-- stable telemetry
                                                +-- fixed network-control ports

ActorSystem::deliver_remote (transitional Phase 3 demultiplexer)
  +-- stream frame ----------------------> existing Phase 4-owned handlers
  +-- ACK/NACK --------------------------> MessagingRuntime typed handler
  +-- backpressure frame ----------------> messaging transitional adapter
  +-- decoded ordinary actor data -------> MessagingRuntime full delivery
```

Ownership and orchestration are deliberately separate. `ActorSystem::Impl`
owns `ActorRuntime`, `MessagingRuntime`, and the still-transitional network and
stream state. It sequences construction/start/stop but does not absorb their
policies.

## 8. `MessagingRuntime` Contract

Recommended private interface:

```cpp
enum class FastDeliveryReason : uint8_t {
    StreamProtocol,
    CompatibilityExplicit,
};

class MessagingRuntime final {
  public:
    struct Dependencies {
        ActorDirectory& actors;
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics;
        MessagingNetworkPorts network;
        EndPoint endpoint;
    };

    struct Config {
        DeadLetterConfig dead_letters;
        adt::DedupCache::Config dedup;
        std::chrono::milliseconds default_message_ttl{0};
        mailbox::ReliableRetryPolicy compatibility_outbound_retry;
    };

    MessagingRuntime(Dependencies dependencies, const Config& config);

    mailbox::EnqueueResult
    try_deliver(ActorId target, TypedMessage message, uint8_t priority,
                int64_t deadline_ns, DeliveryOptions options);

    DeliveryResult
    deliver_with_result(ActorId target, TypedMessage message, uint8_t priority,
                        int64_t deadline_ns, DeliveryOptions options);

    mailbox::EnqueueResult
    try_deliver_fast(ActorId target, TypedMessage message,
                     FastDeliveryReason reason);

    void on_reliable_ack(MessageId message_id, EndPoint endpoint) noexcept;
    void on_reliable_nack(MessageId message_id, EndPoint endpoint,
                          uint32_t reason_code,
                          uint32_t retry_after_ms) noexcept;

    void process_retries(uint64_t now_ns,
                         ReliableRetrySendPort send) noexcept;

    // Transitional Phase 3 seam; removed/replaced by Phase 4 typed routing.
    bool handle_remote_backpressure(const net::WireFrame& frame);

    result<void> reconfigure(const MessagingConfigDelta& delta) noexcept;

    mailbox::DeadLetterQueue& dead_letters() noexcept;
    adt::DedupCache& dedup_cache() noexcept;
    msg::OutboundDeliveryTracker& delivery_receipt_tracker() noexcept;
    mailbox::OutboundTracker& mailbox_reliable_tracker() noexcept;
};
```

Exact existing configuration type names take precedence during implementation;
the example names express responsibility, not permission to redesign public
configuration. Runtime headers remain private under `src/runtime/`.

The internal names deliberately avoid copying the facade's historically
counterintuitive accessor names:

| Existing public facade accessor | Concrete type | Phase 3 internal role |
|---|---|---|
| `outbound_tracker()` | `msg::OutboundDeliveryTracker` | delivery receipt, ACK/NACK, and retry state |
| `reliable_tracker()` | `mailbox::OutboundTracker` | bounded compatibility mailbox tracker |

The facade mappings do not change. Internal code uses role-specific names so a
mechanical ownership move cannot accidentally wire one tracker in place of the
other.

### 8.1 Owned member order

Members are declared in dependency order:

```text
DeadLetterQueue
DedupCache
msg::OutboundDeliveryTracker
mailbox::OutboundTracker
BackpressureCoordinator
DeliveryPipeline
LocalDeliveryEngine
```

C++ destroys them in reverse order, so delivery helpers disappear before the
components they reference. `ActorDirectory` and telemetry are non-owning
construction dependencies and must outlive the entire component.

### 8.2 No monolithic mutex

`MessagingRuntime` is a composition and ownership boundary, not a serialized
actor. It adds no component-wide lock. Each existing component keeps its own
thread-safety contract:

- mailbox producer/consumer synchronization stays in the mailbox;
- dedup synchronization stays in `DedupCache`;
- tracker synchronization stays in each tracker;
- metrics use the existing MPSC ring;
- DLQ synchronization and bounds stay in the queue; and
- the coordinator invokes external actions only after lookups/serialization
  state are locally complete.

## 9. Dependency Injection Without a Container

### 9.1 Direct in-process references

`DeliveryPipeline` receives a concrete dependency bundle:

```cpp
struct DeliveryDependencies {
    ActorDirectory& actors;
    DeadLetterQueue& dead_letters;
    adt::DedupCache& dedup;
    BackpressureCoordinator& backpressure;
    msg::OutboundDeliveryTracker& reliable_tracker;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics;
    ReliableAckPort reliable_ack;
    EndPoint endpoint;
};
```

Actor and mailbox lookup call `ActorDirectory` directly. Local and remote
pressure decisions call `BackpressureCoordinator` directly. This removes four
`std::function` fields and their facade captures while preserving testability
through real focused components.

### 9.2 Fixed network-control ports

Network output is the one unavoidable cross-phase dependency. It uses
non-owning, allocation-free function pointer/context values:

```cpp
struct ReliableAckPort {
    using Emit = void (*)(void* context, const ActorAddress& target,
                          const ActorAddress& acker, uint64_t message_id,
                          uint8_t status, uint32_t retry_after_ms) noexcept;
    void* context{nullptr};
    Emit emit{nullptr};

    void operator()(const ActorAddress& target, const ActorAddress& acker,
                    uint64_t message_id, uint8_t status,
                    uint32_t retry_after_ms) const noexcept;
};

struct BackpressureWirePort {
    using Send = bool (*)(void* context, const ActorAddress& target,
                          const StreamBuffer& encoded) noexcept;
    void* context{nullptr};
    Send send{nullptr};
};

struct MessagingNetworkPorts {
    ReliableAckPort reliable_ack;
    BackpressureWirePort backpressure;
};
```

The context points to stable Phase 1 `NetworkRuntimeState`, not to
`ActorSystem`. That state may report networking unavailable before start or
after stop. Phase 5 moves the same functions onto `NetworkRuntime`; messaging
does not change.

These are control-path ports. Ordinary local mailbox admission performs no
virtual dispatch, service lookup, or port call unless policy actually requires
a control response.

### 9.3 Telemetry

The metrics ring pointer is fixed before the messaging component becomes
reachable. `nullptr` continues to mean telemetry disabled where existing APIs
permit it. No `set_metrics()` method remains on production messaging objects.

## 10. Delivery Path Contracts

### 10.1 Full local delivery

```text
ActorSystem adapter / ActorContext / decoded remote ordinary frame
  -> MessagingRuntime::try_deliver or deliver_with_result
  -> DeliveryPipeline
     -> directory lookup
     -> circuit admission
     -> default TTL/deadline
     -> receiver dedup
     -> mailbox reservation/enqueue
     -> reliable tracking/control response as currently defined
     -> result/DLQ/metrics
     -> pressure decision and signal after enqueue result
```

The implementation preserves the current ordering exactly unless an existing
test proves the documented ordering is different. Policy reordering is not a
refactoring shortcut.

### 10.2 Decoded remote ordinary delivery

Phase 3 keeps decoding in `ActorSystem::deliver_remote()`. Once a frame is
classified as ordinary data, the adapter creates the same `TypedMessage`, trace
context, and `DeliveryOptions`, then invokes the same full entry point as local
delivery. There is no second remote-specific admission pipeline.

### 10.3 Fast local delivery

`try_deliver_fast()` requires a reason and routes to `LocalDeliveryEngine`.
The reason is observable in debug/test instrumentation if practical but does
not alter wire behavior. Phase 3 permits only:

- existing stream protocol handlers, classified `StreamProtocol`; and
- the existing public explicit compatibility API, classified
  `CompatibilityExplicit`.

New internal callers must justify the bypass in code review and architecture
allowlists. The fast path is not an optimization flag for ordinary send.

### 10.4 Result and ownership semantics

- A moved message has one owner at every stage.
- On accepted enqueue, mailbox storage owns the envelope/payload.
- On rejection, the pipeline records or moves data according to current DLQ
  and result rules; it does not reuse a moved payload.
- Facade `void` methods preserve current loss/reporting behavior by calling the
  corresponding internal result path and applying the existing adapter rule.
- No new heap object is allocated merely to forward between facade and
  messaging runtime.

## 11. Reliable Delivery and Backpressure Boundaries

### 11.1 Reliable ACK/NACK

In Phase 3:

```text
wire frame
  -> ActorSystem::deliver_remote parses current flags/fields
  -> MessagingRuntime::on_reliable_ack/on_reliable_nack
  -> msg::OutboundDeliveryTracker
```

Outgoing receiver ACK/NACK emission follows:

```text
DeliveryPipeline policy outcome
  -> ReliableAckPort
  -> stable network state encodes/sends existing WireFrame
```

The accepted-after-handler ACK path used by actor dispatch must resolve the
same messaging-owned port/tracker through a narrow adapter, not by retaining a
facade callback.

### 11.2 Retry timer

The existing network/event-loop timer may continue to own scheduling in Phase
3. Its callback invokes a narrow `MessagingRuntime::process_retries()` method.
It must not fetch a raw tracker and implement tracker policy externally.
Transport resend remains characterized current behavior until a separate
design supplies a complete resend port.

### 11.3 Backpressure

Local pressure uses `ActorDirectory`/`ActorContext` through the coordinator.
Remote pressure uses `BackpressureWirePort`. Incoming frame decoding remains a
transitional coordinator adapter called by `deliver_remote()`; Phase 4 changes
that to a typed decoded signal and owns malformed-frame outcomes.

## 12. Construction, Startup, and Destruction

Required Phase 3 construction order:

1. Construct stable operations/telemetry state.
2. Construct stable transitional network state and its fixed control ports;
   transport may still be unavailable.
3. Construct `ActorRuntime`/`ActorDirectory` without starting workers.
4. Construct `MessagingRuntime` with directory, telemetry, endpoint, and ports.
5. Construct or finish scheduler execution dependencies with
   `messaging.dead_letters()`.
6. Complete actor/scheduler wiring and create reserved actors.
7. Start scheduler, timers, transport, and ingress in the existing approved
   lifecycle order.

Required stop/destruction order:

1. Stop accepting remote ingress and cancel retry/control timers.
2. Drain/stop actor work according to current shutdown policy.
3. Stop and join scheduler workers.
4. Destroy `MessagingRuntime` delivery helpers and state.
5. Destroy `ActorRuntime`/directory and telemetry/network state only after no
   messaging callback can run.

If Phase 2 construction currently needs scheduler creation before final
`ActorRuntime` assembly, transferring an owning `unique_ptr` is allowed only
when the pointee address remains stable and the lifetime inventory proves all
references. No raw dependency may refer to a movable inline member.

## 13. Runtime Configuration and Stable Identity

Phase 3 consumes an already validated effective messaging configuration. It
does not parse TOML. `MessagingRuntime::reconfigure()` accepts only fields that
Phase 0/current behavior classifies as live-reloadable.

Reload rules:

- validate the complete delta before mutation;
- reject immutable/restart-required fields with the existing canonical error;
- update each owning component through its explicit reconfigure API;
- preserve DLQ, dedup, tracker, coordinator, pipeline, and engine object
  identity;
- emit one success/rejection observation; and
- do not partially apply a multi-field delta when validation fails.

Phase 6 replaces the transitional delta assembly with `RuntimeBlueprint` and
formal reload classification. Phase 3 must not grow the monolithic TOML parser.

## 14. Public API and Compatibility Strategy

Preserved facade APIs include:

- all `deliver_local`, `deliver_local_edf`, `try_deliver_local`,
  `try_deliver_local_fast`, and `deliver_with_result` overloads;
- `deliver_remote()` and batch adapters;
- DLQ push/pop/snapshot/accessors;
- dedup and both tracker accessors;
- backpressure signal/handler/test support methods;
- `send_reliable_ack()`; and
- existing constness, defaults, return values, and `noexcept` declarations.

Each method becomes either:

1. a direct forward;
2. a compatibility translation followed by one forward; or
3. for `deliver_remote()`, the transitional Phase 3 frame classification
   adapter described above.

No public header exposes `MessagingRuntime`. Raw accessors return pointers to
the messaging-owned objects and retain stable identity. Deprecation or removal
waits for a later compatibility phase.

## 15. Source and Build Layout

Recommended private runtime files:

```text
src/runtime/messaging_runtime.hpp
src/runtime/messaging_runtime.cpp
src/runtime/messaging_network_ports.hpp
```

Existing component files modified in place:

```text
include/hpactor/mailbox/delivery_pipeline.hpp
src/mailbox/delivery_pipeline.cpp
include/hpactor/mailbox/backpressure_coordinator.hpp
src/mailbox/backpressure_coordinator.cpp
include/hpactor/mailbox/local_delivery_engine.hpp
src/mailbox/local_delivery_engine.cpp
src/runtime/actor_system_impl.hpp
src/runtime/actor_system_impl.cpp
src/actor/actor_system.cpp
```

Private runtime headers are not installed. Focused tests may receive
`${CMAKE_SOURCE_DIR}/src` as a private include directory.

## 16. Incremental Migration Sequence

1. Inventory every delivery, DLQ, dedup, tracker, backpressure, retry-timer,
   fast-path, and late-setter call site; characterize identity and metadata.
2. Add fixed `ReliableAckPort` and `BackpressureWirePort`, bound to stable
   transitional network state, while preserving wire bytes.
3. Replace `DeliveryPipeline` actor/mailbox lookup callbacks with a direct
   `ActorDirectory` dependency.
4. Replace pipeline backpressure callbacks with a direct coordinator reference
   and ACK callback with `ReliableAckPort`.
5. Remove production late metrics/transport setters by fixing construction
   order and stable port context.
6. Introduce `MessagingRuntime` around the existing components in dependency
   order and move ownership without changing algorithms.
7. Route full local delivery, result mapping, DLQ, dedup, tracker, and
   backpressure facade methods through the component.
8. Add explicit fast-delivery reasons and migrate current stream and public
   compatibility callers.
9. Move typed reliable ACK/NACK transitions and retry processing behind
   `MessagingRuntime`; leave frame parsing in the facade.
10. Route decoded ordinary remote data through the same full delivery entry.
11. Connect the Phase 2 scheduler DLQ dependency to the messaging-owned stable
    queue and verify stop/destruction order.
12. Add architecture fitness checks, update lifetime documentation, and run
    focused/full, ASan, and TSAN verification.

Each step is independently buildable. A commit must not move ownership while
leaving a callback that captures the old owner.

## 17. Verification Strategy

### 17.1 Unit tests

- fixed ports: null/unavailable behavior, exact arguments, no facade capture;
- delivery pipeline direct directory lookup and mailbox-not-found behavior;
- full policy ordering for circuit, TTL, dedup, expiry, enqueue, DLQ, metrics,
  reliable tracking, ACK/NACK, and pressure;
- metadata preservation across every result;
- `MessagingRuntime` member identity and accessor stability;
- typed reliable ACK/NACK forwarding;
- both trackers remain independent and bounded;
- fast path requires/classifies a reason; and
- reconfigure preserves object addresses and rejects unsupported deltas.

### 17.2 Integration tests

- all public local delivery overloads preserve outcomes;
- decoded remote ordinary messages use full policy;
- duplicate/expired/missing/circuit-open/full-mailbox paths reach the same DLQ
  and metrics as before;
- local and remote backpressure preserve sender/receiver/state fields;
- stream frame handlers remain on the characterized fast path;
- scheduler expiry and delivery rejection reach one DLQ instance;
- public DLQ/dedup/tracker pointers remain stable across topology reload;
- reliable ACK/NACK frames update the messaging-owned tracker;
- networking-disabled mode safely suppresses remote control output; and
- shutdown cancels retry callbacks before messaging destruction.

### 17.3 Architecture tests

- only `MessagingRuntime` constructs/owns the listed messaging components;
- no production `DeliveryPipeline` callback or lambda captures `ActorSystem`;
- no delivery component stores `ActorSystem*`, `ActorSystem&`, or `Impl*`;
- no production late metrics/transport dependency setter remains;
- ordinary remote data invokes full `MessagingRuntime` delivery;
- fast-engine call sites match a reviewed allowlist;
- facade delivery/DLQ/backpressure methods contain no policy implementation;
  and
- no RTTI, exceptions, generic lookup, virtual hot-path port, or public runtime
  header is introduced.

### 17.4 Sanitizers and stress

- ASan: topology reload followed by dead-lettering, retry timer cancellation,
  failed startup, networking-disabled destruction, and cached accessor use;
- TSAN: concurrent producers, dedup, local/remote pressure, tracker ACK/timer,
  reload of supported live fields, and shutdown while ingress is draining;
- mailbox stress: reservation rollback, full/near-full transitions,
  lost-wakeup prevention, and single-consumer enforcement; and
- repeated lifecycle: construct/start/stop/destruct with and without network.

## 18. Acceptance Criteria

Phase 3 is complete only when:

1. `MessagingRuntime` solely owns the DLQ, dedup cache, delivery pipeline, fast
   local engine, backpressure coordinator, reliable tracker, and compatibility
   tracker.
2. Every public local delivery API is a thin adapter to the messaging owner.
3. Decoded ordinary remote data and ordinary local data use one full-policy
   delivery pipeline.
4. Fast delivery is separately named, reason-classified, and confined to the
   approved compatibility/stream allowlist.
5. Delivery production code stores no complete facade or facade-capturing
   callback.
6. Actor/mailbox lookup and sibling messaging dependencies are concrete fixed
   references; network actions use only narrow fixed ports.
7. No production dependency pointer is repaired through an unsynchronized
   late setter after ingress can begin.
8. DLQ and public messaging object addresses remain stable across reload and
   for all cached consumers.
9. Typed ACK/NACK transitions and retry processing are behind
   `MessagingRuntime`, while frame demultiplexing remains explicitly Phase 4.
10. Message metadata, delivery results, DLQ records, metrics, ACK/NACK,
    backpressure, and tracker behavior match the characterization baseline.
11. Scheduler, mailbox reservation, ready-gate, single-consumer, and
    lost-wakeup concurrency contracts remain intact.
12. Focused/full tests and required ASan/TSAN scenarios pass.

## 19. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Mechanical forwarding drops metadata/defaults | Table-driven parity tests at mailbox, DLQ, ACK, tracker, and metric boundaries |
| DLQ move invalidates scheduler/accessor pointers | One in-place owned object, identity tests, explicit outlives inventory |
| Network port context outlives transport incorrectly | Stable network-state context with lifecycle-aware unavailable result; cancel callbacks before destruction |
| Direct references create a construction cycle | Build telemetry/network state and directory first; inject stable pointees; start only after graph is complete |
| Fast path spreads to ordinary sends | Required reason enum plus architecture allowlist |
| Tracker ownership move is mistaken for reliable resend completion | Preserve and document current behavior; separate reliability issue/design |
| Two trackers are accidentally merged | Distinct names, accessors, tests, and explicit non-goal |
| Backpressure introduces lock inversion | Copy lookup result/release reservation before coordinator or port calls |
| Test sink races production traffic | Pre-start-only contract or coordinator-local synchronization, verified under TSAN |
| Phase 3 absorbs frame/network work | Typed transitional seams and explicit Phase 4/5 handoff criteria |

## 20. Phase 4 and Phase 5 Handoff

Phase 3 ends with messaging ownership complete but remote classification still
transitional.

Phase 4 constructs `InboundFrameRouter` with:

- `MessagingRuntime&` for ordinary delivery and typed reliable control;
- a typed backpressure handler exposed by the messaging-owned coordinator;
- `StreamRuntime&` for stream protocol frames; and
- RPC/unsupported-frame handlers owned by their proper subsystems.

It removes frame parsing and stream maps from `ActorSystem` without moving any
messaging state again.

Phase 5 replaces transitional `NetworkRuntimeState` port contexts with the
final `NetworkRuntime` implementation. Because the `ReliableAckPort` and
`BackpressureWirePort` contracts are already narrow, that change affects
composition wiring rather than delivery policy.

No Phase 3 implementation should create an inbound router, take ownership of a
transport/event loop, or move stream protocol state merely to make the
component appear more complete.
