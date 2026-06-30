<!--
Copyright 2026 HPActor Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Fixed-Message Disruptor Actor Mailbox Design

**Status:** Approved design, implementation pending

**Date:** 2026-06-30

**Audience:** HPActor runtime, mailbox, scheduler, and performance engineers

## 1. Executive Summary

HPActor's default `MPSCActorMailbox<TypedMessage>` is optimized for dynamic,
variable-length actor traffic. It combines intrusive MPSC queues, priority
lanes, bounded admission, overflow policies, lifecycle behavior, and production
observability. A successful enqueue still allocates a message node, follows
pointer-linked storage, and eventually destroys and deallocates that node.

Actors whose user-message set is known at compile time and whose messages have
fixed object representation can avoid those costs. This design adds an opt-in
`FixedMailboxActor<Capacity, Messages...>` backed by a preallocated,
LMAX-Disruptor-style multi-producer/single-consumer ring. User messages are
stored directly in fixed ring slots. The actor retains a conventional protected
MPSC lane for variable-length lifecycle and system-control `TypedMessage`s.

The default actor mailbox is unchanged. The fixed backend is local-only in
version 1, uses a compile-time closed message set, rejects on full without
blocking or eviction, and remains experimental until it passes explicit
correctness and performance gates.

## 2. Current State and Motivation

### 2.1 Existing actor mailbox

`MPSCActorMailbox<TypedMessage>` currently owns or coordinates:

- one protected system lane and up to eight user-priority lanes;
- intrusive `MPSCMailbox<T>` storage through `MultiLaneQueue<T>`;
- per-message allocation through the actor-attributed message region;
- bounded count and byte reservations;
- admission policies, rate limiting, overflow handlers, and pressure state;
- scheduler notification on the empty-to-non-empty edge;
- delivery metrics, logging, DLQ handoff, and snapshots; and
- serialized consumer-side dequeue, eviction, and overflow draining.

This is the correct general-purpose backend for `TypedMessage`, which can own a
variable-length `StreamBuffer` and a shared protobuf object. Its flexibility
also means the user-message hot path cannot be reduced to a fixed address,
fixed-size store followed by sequence publication.

### 2.2 Existing ring buffers are not an actor mailbox

`MpscRingBuffer<T>` and `DynamicMpscRingBuffer<T>` already provide bounded
MPSC telemetry storage. They are not reused directly because their contract is
bulk-drain/copy oriented and lacks:

- a consumer lease that keeps an in-place actor message alive through handler
  dispatch;
- a shared wakeup gate spanning a system lane and user ring;
- actor lifecycle closure and drain semantics;
- fixed-message compile-time admission;
- mailbox pressure and delivery results;
- claimed-but-unpublished gap diagnostics; and
- scheduler execution integration.

The new primitive may reuse their per-slot sequence technique, but it has a
separate actor-message ownership contract.

### 2.3 Intended performance win

The fixed user path removes:

- one message-node allocation and deallocation per delivery;
- intrusive pointer chasing;
- deferred node reclamation;
- byte-size estimation for a variable payload; and
- runtime message-type acceptance checks after a typed reference has compiled.

The design does not assume that a ring is universally faster. Publication
cursor contention and producer preemption can make the ring worse under some
fan-in patterns. The backend ships only if measured results satisfy Section 15.

## 3. Goals

1. Provide a local, opt-in actor mailbox for a compile-time closed set of
   fixed-size user-message types.
2. Store user messages directly in preallocated ring slots with zero
   per-message allocation or deallocation.
3. Preserve HPActor's single-consumer actor execution model and ready-gate
   state transitions.
4. Preserve lifecycle, quarantine, deadline, tracing, bounded admission,
   backpressure, failure, metrics, logging, and CLI visibility appropriate to
   the version 1 delivery contract.
5. Keep lifecycle and system-control messages compatible by retaining a
   protected MPSC system lane.
6. Leave existing actors and `MPSCActorMailbox<TypedMessage>` source-compatible
   and behavior-compatible by default.
7. State the ring's linearization points, memory ordering, ownership, progress,
   and stalled-producer behavior precisely.
8. Require deterministic, model-checked, stress, sanitizer, allocation, and
   comparative benchmark evidence before the backend is considered stable.

## 4. Non-Goals

Version 1 does not provide:

- remote delivery or a fixed-message wire codec;
- dynamic or protobuf user messages in the ring;
- ask/reply correlation;
- reliable delivery, ACK/NACK, retry, or receiver deduplication;
- priority user lanes or EDF placement;
- producer blocking or waiting for capacity;
- `DropOldest`, `DropLowestPriority`, spill, or producer-side eviction;
- replayable fixed-message DLQ payloads;
- live capacity changes or ring replacement;
- coroutine mailbox awaiting for fixed user messages;
- TOML selection of the backend for an actor type not compiled for it; or
- replacement of the general-purpose mailbox.

These exclusions are contractual. Unsupported paths return an explicit error
or fail at compile time; they do not silently fall back to the variable-length
mailbox.

## 5. Chosen Architecture

Three architectures were considered:

1. **Hybrid fixed actor mailbox:** protected MPSC system lane plus Disruptor user
   ring.
2. **All-ring mailbox:** fixed variants for both system and user traffic.
3. **Runtime type-erased interchangeable mailbox:** one virtual or indirect
   interface for every mailbox backend.

The hybrid mailbox is selected. It isolates the optimized path to user traffic,
does not couple the ring ABI to every system-message evolution, and avoids
adding runtime dispatch to the existing MPSC hot path. Actor directory and
scheduler records carry a mailbox-kind discriminator. Existing actors continue
to use their concrete MPSC pointers; only fixed actors use narrow function
pointer/context ports.

```text
FixedActorRef<Capacity, Messages...>
                |
                | local fixed envelope
                v
      +--------------------------+
      | FixedActorMailboxCore    |
      |                          |
      |  protected MPSC lane <------- existing TypedMessage control delivery
      |  Disruptor MPSC ring     |<-- fixed user delivery
      |  shared wakeup gate      |
      |  admission/metrics state |
      +-------------+------------+
                    |
                    | one serialized consumer
                    v
      FixedMailboxActor<Capacity, Messages...>
                    |
                    v
          ActorExecutionEngine
```

## 6. Public Actor and Reference API

### 6.1 Fixed-message concept

```cpp
template <typename T>
concept FixedMailboxMessage =
    std::is_standard_layout_v<T> &&
    std::is_trivially_default_constructible_v<T> &&
    std::is_trivially_copyable_v<T> &&
    std::is_trivially_destructible_v<T>;
```

The concept makes copying into a claimed slot non-throwing and prevents an
owning `std::string`, `std::vector`, protobuf object, or other variable-length
RAII value from masquerading as a fixed message. Trivial default construction
also lets every preallocated variant slot start in a valid state without a
special empty alternative. Raw pointers may technically satisfy the concept but
remain subject to the actor rule that mutable shared state must not cross actor
boundaries. Public documentation recommends IDs, offsets, handles, or immutable
views with externally guaranteed lifetime.

Every message type must be unique in `Messages...`. `Capacity` must be at least
2, no greater than `1 << 20`, and a power of two. Spawn performs checked
multiplication for total ring bytes and applies memory-region admission before
constructing the core.

### 6.2 Actor declaration

```cpp
struct Quote {
    uint64_t instrument;
    int64_t price;
    uint32_t quantity;
};

struct CancelQuote {
    uint64_t instrument;
};

class QuoteActor final
    : public FixedMailboxActor<1024, Quote, CancelQuote> {
  public:
    using FixedMailboxActor::FixedMailboxActor;

  protected:
    fixed_behavior_type make_fixed_behavior() override {
        return {
            on_fixed<Quote>([this](const Quote& msg) { handle_quote(msg); }),
            on_fixed<CancelQuote>(
                [this](const CancelQuote& msg) { handle_cancel(msg); }),
        };
    }

  private:
    void handle_quote(const Quote& msg);
    void handle_cancel(const CancelQuote& msg);
};
```

`FixedMailboxActor` derives from `EventBasedActor` to retain actor state,
lifecycle, supervision, drain, system-message dispatch, logging, and metrics.
It owns a fixed behavior table and dispatches the closed user variant through
`std::visit`. Handler objects may allocate during actor activation; dispatch and
message storage do not allocate per message.

### 6.3 Spawn and reference

```cpp
auto quote_ref = system.spawn_fixed<QuoteActor>(spawn_options);

auto result = context()->try_send(quote_ref, Quote{42, 10125, 10});
if (!result.accepted()) {
    // Apply caller policy.
}
```

`spawn_fixed<T>()` returns the exact `T::fixed_actor_ref_type`. A fixed reference
contains:

- the local actor address and incarnation;
- a `shared_ptr` to the stable fixed mailbox core; and
- no owning reference to the actor object.

The mailbox core may outlive the actor object. Actor termination closes the
core before directory unpublication and actor destruction. A surviving
reference therefore returns `MailboxClosed` rather than dereferencing an actor.
Holding a fixed reference does not keep the actor alive.

Holding a fixed reference does retain the closed core and its ring storage.
This is a deliberate version 1 lifetime tradeoff that avoids a refcount or
hazard-pointer operation on every send. Closed-core bytes remain visible in
memory accounting until the last reference is released. Spawn limits and
operations metrics must make that retained bounded memory observable. A future
small-route-shell design may reclaim ring storage earlier only if it preserves
safe concurrent send teardown without adding a larger hot-path cost.

`FixedActorRef::try_send()` supports non-actor callers with an empty sender and
no current trace. `ActorContext::try_send()` overloads populate sender address,
current trace, deadline, message ID, and flags. Only types present in the
closed set participate in overload resolution.

There is no fire-and-forget `send()` in version 1. Callers must observe the
bounded `EnqueueResult`.

## 7. Fixed Envelope

```cpp
template <FixedMailboxMessage... Messages>
struct FixedMessageEnvelope {
    using message_type = std::variant<Messages...>;

    message_type message;
    ActorAddress sender;
    TraceContext trace;
    int64_t deadline_ns = INT64_MAX;
    uint64_t message_id = 0;
    uint64_t enqueue_sequence = 0;
    uint32_t flags = 0;
    bool has_trace = false;
};
```

The envelope has a compile-time fixed size equal to the largest alternative
plus the variant discriminator and fixed metadata. It contains no dynamic
payload or shared protobuf pointer. Padding bytes are never exported, logged,
hashed, or copied into the DLQ.

Priority is omitted because version 1 has one FIFO user ring. EDF scheduling is
also omitted because one activation may be triggered by several queued user
messages with different deadlines. Deadline expiry is still enforced before
publication and immediately before dispatch.

## 8. Disruptor Ring Protocol

### 8.1 Structure

```cpp
template <typename T, size_t Capacity>
class DisruptorMpscRing {
    struct Slot {
        std::atomic<uint64_t> sequence;
        T value;
    };

    alignas(64) std::atomic<uint64_t> claim_cursor_{0};
    alignas(64) uint64_t consumer_sequence_{0};
    alignas(64) RingDiagnostics diagnostics_;
    std::array<Slot, Capacity> slots_;
};
```

The cursor and diagnostics occupy separate cache lines. Slots use natural
envelope alignment rather than unconditional 64-byte padding; the benchmark
must report slot size and total ring footprint. A future layout optimization
requires new performance and concurrency evidence.

At initialization, slot `i` has `sequence == i`.

### 8.2 Producer protocol

For logical producer sequence `p`:

1. Load `claim_cursor_` relaxed.
2. Inspect slot `p & (Capacity - 1)` with an acquire load.
3. If its sequence does not equal `p`, return `Rejected` as full. Do not spin.
4. CAS `claim_cursor_` from `p` to `p + 1` with relaxed success ordering. On
   CAS failure, retry from the updated cursor.
5. Copy the complete fixed envelope into the exclusively claimed slot.
6. Publish with `slot.sequence.store(p + 1, memory_order_release)`.
7. Arm the mailbox wakeup gate after publication.

The claim CAS is the reservation linearization point. The release store is the
publication/visibility linearization point. An accepted result is returned only
after publication.

No operation that can fail, throw, allocate, log synchronously, call user code,
or abandon execution is allowed between claim and publication. Fault injection
may reject before claim or observe after publication, but it may not inject a
failure after claim.

### 8.3 Consumer protocol

For logical consumer sequence `c`:

1. Acquire-load slot `c & mask`.
2. If its sequence is not `c + 1`, report no consumable user message.
3. Return a move-only `ReadLease` referencing the in-place envelope.
4. Dispatch the envelope while the lease remains alive.
5. On lease destruction, release-store `c + Capacity` into the slot sequence.
6. Increment the consumer sequence.

Only the actor execution context may acquire or release a read lease. No
producer may overwrite, evict, inspect, or release a consumer-owned slot.

### 8.4 Memory-order proof obligations

- The producer's release publication synchronizes with the consumer's acquire
  readiness check, so all envelope writes happen-before dispatch.
- The consumer's release of a slot synchronizes with a future producer's
  acquire availability check, so dispatch completes before overwrite.
- The claim CAS assigns each logical sequence to exactly one producer.
- A slot cannot be reclaimed until its sequence advances by `Capacity`.
- The single consumer needs no atomic consumer cursor for ownership; snapshots
  read a mirrored atomic value updated after release.

These obligations must be encoded in Relacy tests before production code is
accepted.

### 8.5 Progress guarantee and producer gaps

Producer admission is lock-free but not wait-free because CAS contention can
force retries. Full detection is bounded and does not wait for the consumer.

A producer preempted after claim and before publication creates a FIFO gap. The
consumer does not spin and does not skip the gap, even if later producers have
published later sequences. Later messages become consumable after the missing
producer publishes. Consequently, consumer progress is obstruction-sensitive
to a stalled claimed producer. This is an explicit multi-producer Disruptor
tradeoff, not described as wait-free behavior.

Diagnostics expose the distance between the claim cursor and the next
consumable sequence, plus the number of observed claimed-but-unpublished gaps.

## 9. Hybrid Mailbox Core

`FixedActorMailboxCore<Capacity, Messages...>` owns:

- `DisruptorMpscRing<FixedMessageEnvelope<Messages...>, Capacity>`;
- a protected `MPSCMailbox<TypedMessage>` system lane;
- system-lane reservation and deferred node-reclamation state;
- one shared edge-trigger wakeup gate;
- immutable delivery and execution ports;
- watermarks, pressure state, counters, metrics, and logging pointers; and
- atomic lifecycle flags: open/closed and accepting/rejecting user traffic.

The core is fully wired before publication to the actor directory. No setter
may replace its ring, scheduler, actor ID, delivery port, or execution port
after spawn.

The core also owns an atomic in-flight publisher count. A sender increments the
count before its final `accepting_user` check and decrements it only after
publication or rejection. Drain/close first changes `accepting_user` to false,
then waits asynchronously for the count to reach zero before declaring user
admission quiescent. The scheduler thread does not busy-wait or block; normal
drain progress, a timer, or the publisher's completion notification drives the
recheck. A sender that joined the in-flight set before the close transition may
finish publication and is part of the drain set. A sender that joins afterward
observes false and cannot claim a slot.

### 9.1 Single-consumer rule

The actor's `Ready -> Running` CAS grants the only consumer right. Normal
dispatch, immediate drain, and shutdown drain execute under that same actor
activation right. The fixed backend has no producer-side eviction and therefore
needs no producer-accessible consumer lock.

One `consume_one()` call:

1. checks and consumes the protected system lane;
2. otherwise acquires the next published user slot;
3. rejects an expired user envelope through the failure path; or
4. dispatches the user variant while holding its read lease.

System-first ordering matches the existing mailbox contract. FIFO is guaranteed
within the user ring. There is no global order between the system and user
lanes.

### 9.2 Shared edge-trigger protocol

Both lanes publish before arming the same atomic `work_signaled` gate. The first
producer in a quiet period changes the gate from false to true and calls the
scheduler ready port. Later producers observe true and do not add duplicate
runnable entries.

When the consumer finds neither a system message nor its next user sequence:

1. clear `work_signaled` with release semantics;
2. recheck both lanes with acquire semantics;
3. if work appeared, re-arm the gate and requeue through the owned-admission
   path;
4. otherwise transition the actor to `Idle`; and
5. perform the existing post-idle double-check and external ready admission.

The deterministic lost-wakeup tests must cover a producer at every boundary in
this sequence for both lanes.

## 10. Runtime Integration

### 10.1 Actor directory

The actor directory record gains a `MailboxKind` discriminator:

```cpp
enum class MailboxKind : uint8_t {
    VariableMpsc,
    FixedDisruptor,
};
```

Variable actors retain their concrete `MPSCActorMailbox<TypedMessage>*` fast
path. Fixed actors register narrow RTTI-free ports containing `void*` context
and function pointers. The directory atomically publishes the actor, name,
mailbox kind, and matching ingress/execution data as one adoption operation.

### 10.2 Fixed delivery port

The fixed mailbox core receives an immutable port to `MessagingRuntime` for
non-template preflight and outcome reporting:

```cpp
struct FixedDeliveryPort {
    void* context;
    DeliveryPreflightResult (*preflight)(void*, ActorId,
                                         const FixedEnvelopeMeta&) noexcept;
    void (*record_accepted)(void*, ActorId,
                            const FixedDeliveryObservation&) noexcept;
    void (*record_rejected)(void*, ActorId,
                            const FixedDeliveryFailure&) noexcept;
};
```

Preflight performs actor/lifecycle, quarantine/circuit, and already-expired
checks before ring claim. The core performs bounded ring admission. Outcome
callbacks update metrics, structured failure information, pressure signaling,
logging, and metadata-only DLQ evidence. The callbacks are non-blocking and may
not invoke user code.

This port preserves the supported production delivery semantics without
serializing a fixed message into `TypedMessage`. It explicitly does not provide
the non-goals in Section 4.

### 10.3 System/control ingress

Fixed actor directory records expose a `TypedMessage` control-ingress function.
It accepts only TypeTags below `TypeTag::User`. A user `TypedMessage` targeting a
fixed actor returns `FailureReason::UnsupportedMessageType`; it is not routed to
the ring and does not fall back.

System ingress allocates one conventional `TypedMessage` node, applies the
protected system-message limit, publishes to the MPSC system lane, then arms
the shared wakeup gate. Its ownership and deferred reclamation follow the
existing MPSC rules.

### 10.4 Scheduler execution port

Fixed actor records expose:

```cpp
struct FixedMailboxExecutionPort {
    void* context;
    ActorRunResult (*consume_one)(void*, EventBasedActor&,
                                  const ActorExecutionContext&) noexcept;
    bool (*empty)(const void*) noexcept;
    cli::MboxSnapshot (*snapshot)(const void*) noexcept;
};
```

`ActorExecutionEngine` switches on `MailboxKind` once per activation. Existing
actors continue through `BehaviorActorRunner`. Fixed actors use a focused
`FixedMailboxActorRunner` that shares the existing `Ready -> Running`, requeue
budget, idle transition, and lost-wakeup rules. The port prevents RTTI,
`dynamic_cast`, and a virtual mailbox hierarchy.

## 11. Delivery and Failure Semantics

### 11.1 Supported delivery

Version 1 supports local observable best-effort delivery. An accepted result
means the complete fixed envelope was published to the ring. It does not mean
the actor handler has executed.

Before publication, delivery can fail because the target is remote, mailbox is
closed, actor is draining or terminated, actor is quarantined, circuit is open,
deadline is already expired, or ring is full. Each path returns an explicit
`EnqueueResult` and canonical failure reason.

The implementation adds precise failure reasons where existing values cannot
represent:

- fixed mailbox remote delivery unsupported;
- unsupported dynamic user message; and
- fixed ring full.

Failure-reason additions must preserve numeric compatibility and receive new
CLI mapping tests.

### 11.2 Full ring

The only ring overflow behavior is `RejectNewest`:

- no producer waits;
- no claimed slot is abandoned;
- no existing slot is evicted;
- no message is spilled by the ring; and
- no retry occurs inside the mailbox.

The rejection result includes current depth, capacity, pressure state, pressure
ratio, and suggested retry delay. The caller decides whether and when to retry.

### 11.3 Deadline

Deadline is checked twice:

1. before claim, to avoid occupying a slot with an already-expired message;
2. after consumer acquisition, because a valid message may expire while queued.

Consumer-side expiry releases the lease without invoking the user handler and
emits the same delivery failure, trace correlation, metric, and metadata-only
DLQ evidence as producer-side expiry.

### 11.4 DLQ

Fixed-message DLQ entries are metadata-only and non-replayable in version 1.
They include actor, sender, fixed type name/index, message ID, trace, deadline,
timestamp, size, and reason. They do not copy object representation because
padding may be uninitialized and local C++ layout is not a stable serialization
format.

## 12. Lifecycle and Shutdown

- **Spawn:** allocate and initialize the core, validate memory admission, bind
  immutable ports, bind the actor, then atomically publish the directory record.
- **Running:** system and user traffic may be admitted subject to their
  respective bounds.
- **Draining:** atomically stop new user admission. Continue accepting protected
  control traffic. Existing user slots follow `DrainPolicy`.
- **Immediate stop:** the single actor consumer releases all published slots
  without invoking handlers and records drop evidence.
- **Complete drain:** consume existing user slots normally, with deadline checks.
- **Timeout drain:** consume until deadline, then release remaining slots as
  drain-timeout failures.
- **Quarantined/circuit open:** preflight rejects user traffic; system traffic
  remains available for recovery and operations.
- **Termination:** close user admission, finish the selected drain policy,
  unpublish the directory route, mark the shared core closed, and destroy the
  actor. Surviving references can only observe closed rejection.

No actor handler, transport call, blocking log sink, or user callback runs while
directory publication locks or mailbox lifecycle locks are held.

## 13. Configuration and Compatibility

The message set and ring capacity are compile-time properties of the actor
type. TOML cannot resize the ring or turn an arbitrary actor into a fixed actor.

Applicable runtime settings are limited to:

- high, low, and critical pressure watermarks;
- backpressure signal mode and minimum interval; and
- protected system-message capacity.

Global variable-mailbox defaults do not constrain a fixed actor; in particular,
the global default of four priority levels is not copied into the fixed core.
An explicit per-actor override that requests priority-aware lanes, a priority
level count other than one, an overflow policy other than `RejectNewest`, a
nonzero overflow depth, or a different capacity produces a validation error
rather than being silently ignored.

The default spawn APIs, `EventBasedActor`, and
`MPSCActorMailbox<TypedMessage>` remain source-compatible. Existing topology
actors retain the variable MPSC backend unless their registered factory creates
a compiled fixed actor type.

## 14. Memory Accounting, Observability, and Fault Injection

### 14.1 Memory

Spawn charges the complete ring allocation once to
`mem::RegionType::kMessage`, attributed to the actor ID. Checked arithmetic
prevents capacity-by-slot-size overflow. A failed region admission aborts spawn
with an explicit resource failure. Ring destruction returns the entire charge.

The fixed user send/consume path performs zero calls to `mem::allocate`,
`mem::deallocate`, global `new`, or global `delete` after spawn.

### 14.2 Snapshot and metrics

`MboxSnapshot` gains backend-aware fields:

- mailbox kind;
- fixed ring capacity and slot bytes;
- published/consumable depth;
- maximum observed depth;
- accepted and rejected totals;
- claim CAS retry total;
- claimed-but-unpublished gap observations;
- system-lane depth; and
- current pressure state and ratio.

Snapshots are approximate under concurrent producers and stable only while the
mailbox is quiesced. CLI actor inspection and metrics expose the same bounded
data. Existing generic mailbox fields retain their meaning where applicable.

### 14.3 Logging and tracing

Accepted and rejected delivery metrics carry backend kind. Rejection logs are
rate-limited by the existing pressure signaling rules. Trace context is copied
into the fixed envelope and becomes the parent context for handler spans.

### 14.4 Fault injection

Allowed fault points are:

- before claim: fail/reject without state change;
- after publication: delay observation, emit diagnostics, or perturb wakeup
  timing while preserving eventual notification; and
- consumer before dispatch: delay or mark expiry without abandoning the lease.

No failure, drop, panic, or early-return hook may execute between claim and
publication. Tests may deliberately delay there to model preemption, but the
test producer must eventually publish.

## 15. Verification Strategy and Acceptance Gates

### 15.1 Compile-time tests

- capacity rejects zero, one, non-power-of-two, and greater-than-maximum values;
- duplicate alternatives are rejected;
- variable-length and nontrivial message types are rejected;
- declared messages are sendable; and
- undeclared messages do not satisfy the `try_send` overload.

Negative compile checks use CMake `try_compile` fixtures rather than source that
breaks the normal test target.

### 15.2 Ring unit tests

- initialization and empty state;
- one producer FIFO;
- multiple producer unique claims;
- exact capacity and full rejection;
- wraparound over several capacity cycles;
- read-lease lifetime and release;
- no overwrite before release;
- closed-ring rejection;
- variant alternative integrity; and
- counter and snapshot behavior while quiesced.

### 15.3 Model and concurrency tests

Relacy models prove:

- unique claim assignment;
- publish-to-consume happens-before;
- release-to-reuse happens-before;
- full detection never overwrites a live slot;
- the consumer never observes a partial envelope; and
- edge-trigger clear/recheck cannot lose a wakeup.

A controlled stalled-producer test claims sequence `n`, delays publication,
publishes `n + 1` from another producer, verifies that the consumer does not
skip `n`, then publishes `n` and verifies FIFO completion.

Stress tests run 1, 2, 4, and 8 producers against one consumer. Every attempt
must be accounted as consumed or rejected, every consumed sequence must be
unique, and depth must never exceed capacity.

### 15.4 Mailbox and actor integration tests

- system messages dispatch before user messages;
- either lane can create the empty-to-non-empty edge;
- producer races at every clear/recheck/idle boundary do not lose wakeups;
- full-ring results include exact bounded admission data;
- producer-side and consumer-side deadline expiry;
- lifecycle, quarantine, drain, close, and surviving-reference behavior;
- trace and sender propagation;
- metadata-only, non-replayable DLQ evidence;
- fixed and variable actors coexist under the scheduler; and
- requeue budget prevents a hot fixed actor from monopolizing a worker.

### 15.5 Allocation and sanitizer tests

After spawn and behavior initialization, a sustained accepted-message run must
record zero message-region allocations and frees. System-control traffic is
measured separately because its protected MPSC lane intentionally allocates.

Run ASan and TSan where supported. Platform sanitizer limitations must be
recorded as environmental evidence, not converted into a passing claim.

### 15.6 Comparative benchmark

The benchmark compares `FixedActorMailbox` with
`MPSCActorMailbox<TypedMessage>` under equivalent local observable best-effort
semantics, one user lane, `RejectNewest`, identical scheduler settings, and the
same metrics/tracing mode.

Matrix:

- payload sizes: 32, 64, 128, and 256 bytes;
- producers: 1, 2, 4, and 8;
- capacities: 1,024 and 65,536;
- steady-state occupancy and burst-to-full workloads; and
- observability disabled and enabled.

Report:

- accepted messages per second;
- enqueue p50/p95/p99;
- end-to-end handler p50/p95/p99;
- rejection rate;
- claim CAS retries;
- producer-gap observations;
- CPU time and scheduler fairness;
- allocations per accepted message; and
- mailbox bytes per actor.

Each result is the median of seven release-build runs on the same otherwise idle
host. Record compiler, flags, OS, CPU, core count, governor/power mode, payload,
capacity, producer count, scheduler threads, and enabled instrumentation.

The backend remains experimental unless the canonical 64-byte workload shows:

1. at least 20% higher throughput with one producer;
2. at least 10% higher throughput with four producers;
3. no worse p99 enqueue latency in non-overflow runs;
4. zero per-message allocation; and
5. no scheduler fairness regression greater than 5% in a mixed fixed/MPSC
   actor workload.

Benchmark thresholds are review gates, not timing assertions in CI unit tests.

## 16. File and Component Boundaries

Planned components:

| Component | Responsibility |
|---|---|
| `disruptor_mpsc_ring.hpp` | Fixed-slot claim, publish, lease, release, and diagnostics |
| `fixed_message_envelope.hpp` | Closed fixed variant and delivery metadata |
| `fixed_actor_mailbox.hpp` | Hybrid system lane, user ring, readiness, pressure, and lifecycle |
| `fixed_mailbox_actor.hpp` | Actor base, fixed behavior registration, and variant dispatch |
| `fixed_actor_ref.hpp` | Typed local reference and non-blocking send API |
| `fixed_mailbox_ports.hpp` | RTTI-free delivery, control-ingress, and execution ports |
| `fixed_mailbox_actor_runner.*` | Scheduler activation for fixed actors |
| existing actor directory/runtime files | Atomic publication and mailbox-kind routing |
| existing snapshots/metrics/CLI files | Backend-aware operational visibility |
| benchmark app | Comparative MPSC versus fixed-ring evidence |

Each component has one owner. The ring knows nothing about actors, scheduling,
DLQ, or logging. The mailbox core owns queue composition and readiness. The
actor owns behavior dispatch. Runtime ports own cross-subsystem integration.

## 17. Rollout

1. Land the standalone ring with model and unit tests.
2. Land the fixed envelope and hybrid mailbox core with deterministic wakeup
   tests.
3. Land actor/ref APIs and local spawn/delivery integration behind
   `ENABLE_FIXED_DISRUPTOR_MAILBOX` (default `OFF` while experimental).
4. Land lifecycle, observability, CLI, allocation, and coexistence tests.
5. Land the benchmark and collect review evidence.
6. Change the build option default only after every correctness gate and the
   Section 15 performance thresholds pass on documented reference hardware.

No phase changes the default mailbox of existing actors.

## 18. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Preempted producer blocks FIFO progress | Keep claim-to-publish region trivial and bounded; expose gap diagnostics; benchmark fan-in |
| Publication cursor contention erases gains | Benchmark 1/2/4/8 producers; retain MPSC default; require explicit thresholds |
| Ring memory footprint is excessive per actor | Compile-time bound, checked allocation, region admission, footprint metrics |
| Duplicate scheduler admission across two lanes | One mailbox-owned wakeup gate plus ready-state CAS and deterministic race tests |
| Fixed path bypasses production delivery semantics | Immutable MessagingRuntime preflight/outcome port and explicit non-goals |
| Actor dies while references survive | Shared mailbox core, close-before-destroy, no actor pointer in reference |
| Raw fixed bytes leak padding or ABI details | Never serialize/log/DLQ-copy object representation |
| Runtime mailbox abstraction slows existing actors | Mailbox-kind branch per activation; keep concrete MPSC fast path unchanged |
| System traffic starves user ring | Preserve current system-first contract; bound protected lane; measure fairness |

## 19. Design Decisions

1. The backend is opt-in per actor; MPSC remains the default.
2. User traffic is a compile-time closed fixed-message set.
3. Version 1 is local-only.
4. The mailbox is hybrid: MPSC system lane plus Disruptor user ring.
5. Full-ring behavior is non-blocking `RejectNewest` only.
6. A read lease owns a ring slot through handler dispatch.
7. Existing actors do not pay a virtual-mailbox or function-pointer cost on
   their current hot path.
8. Fixed-message DLQ evidence is metadata-only and non-replayable.
9. The feature remains experimental until both correctness and performance
   gates pass.
