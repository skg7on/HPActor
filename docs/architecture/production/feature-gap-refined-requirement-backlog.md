# Feature Gap Refined Architecture Requirement Backlog

## 1. Purpose

This backlog refines the "Feature Gaps By Subsystem" review into architecture
requirements that can feed design specs, implementation plans, and release
milestones. It is more detailed than the summary backlog in
`architecture-requirement-backlog.md`.

Each requirement describes:

- the production gap it closes
- the architecture requirement
- the target runtime contract
- dependencies
- acceptance evidence
- required observability
- required test coverage

## 2. Requirement Metadata

Priority:

- `P0`: required before HPActor can be considered safe for 24x7 production use.
- `P1`: required for scalable multi-node production.
- `P2`: required for mature platform operation and developer ergonomics.

Status:

- `Missing`: no implementation exists.
- `Partial`: some related code exists, but the production contract is not
  complete.
- `Designed`: architecture design exists, but implementation is incomplete.
- `Planned`: implementation plan exists or is being prepared.

Release lane:

- `Foundation`: single-node and basic distributed reliability.
- `Cluster`: multi-node correctness and scaling.
- `Operations`: secure and observable production operation.
- `Durability`: opt-in persistence and reliable recovery.
- `Maturity`: automation, compatibility, and ecosystem polish.

## 3. Executive Priority Map

| Lane | Must land first | Why |
|------|-----------------|-----|
| Foundation | Delivery semantics, bounded mailboxes, DLQ, tracing, health, graceful shutdown | Prevents silent loss, OOM, and unobservable failure. |
| Cluster | failure model, protocol negotiation, route invalidation, sharding, placement | Makes multi-node operation predictable under node loss. |
| Operations | security, admin API, audit, config validation, incident timeline | Lets humans and automation run the system safely. |
| Durability | reliable messaging, durable state, recovery, passivation | Supports critical actors and messages after restart. |
| Maturity | chaos tests, fuzzing, compatibility, deployment guides, autoscaling | Proves and sustains production quality over time. |

## 4. Actor Runtime Requirements

### AR-001: Actor Lifecycle State Machine

Priority: `P0`
Status: `Partial`
Release lane: `Foundation`

Gap:

The runtime has actor states, lifecycle hooks, supervision, hibernation, and
termination paths, but there is no single production lifecycle contract shared
by message delivery, shutdown, supervision, passivation, metrics, CLI, and
durable recovery.

Architecture requirement:

Define and implement a canonical actor lifecycle state machine:

- `Constructed`
- `Starting`
- `Active`
- `Draining`
- `Stopping`
- `Stopped`
- `Failed`
- `Restarting`
- `Hibernating`
- `Passivated`
- `Recovering`

Runtime contract:

- Every actor has exactly one visible lifecycle state.
- Message admission checks the lifecycle state before enqueue.
- Supervision transitions actors through restart states instead of ad hoc
  replacement.
- CLI and metrics expose lifecycle state.
- Durable recovery must complete before transition to `Active`.

Dependencies:

- Delivery result contract.
- Graceful shutdown protocol.
- Durable actor state for `Recovering`.
- Passivation design for `Passivated`.

Acceptance evidence:

- State transition table is documented and covered by tests.
- Illegal transitions are rejected or logged as runtime defects.
- Message delivery to non-active states returns typed failure or follows drain
  policy.
- CLI can inspect lifecycle state for any actor.

Observability:

- `hpactor_actor_lifecycle_state`
- `hpactor_actor_lifecycle_transitions_total`
- structured lifecycle transition logs with actor id and reason

Tests:

- lifecycle transition unit tests
- supervision restart transition tests
- shutdown drain transition tests
- durable recovery activation tests

### AR-002: Graceful Actor Stop And Drain Policy

Priority: `P0`
Status: `Missing`
Release lane: `Foundation`

Gap:

Actors do not have a unified stop protocol that decides what happens to in-flight
work, queued messages, system messages, and linked actors during shutdown or
rolling upgrade.

Architecture requirement:

Add actor drain policy:

- `Drain`: process queued user messages until empty or timeout.
- `DropUserMessages`: keep protected system messages, DLQ user messages.
- `SnapshotAndStop`: persist state then stop.
- `TransferShard`: hand off sharded ownership before stop.
- `ImmediateStop`: stop without user-message drain.

Runtime contract:

- Stop cannot block forever.
- System control messages required for shutdown remain deliverable.
- Dropped messages are dead-lettered with shutdown reason.
- Actors expose drain progress to node shutdown.

Dependencies:

- Dead-letter queue.
- Actor lifecycle state machine.
- Graceful node shutdown.
- Durable actor state for `SnapshotAndStop`.
- Shard handoff for `TransferShard`.

Acceptance evidence:

- Per-actor policy can be configured.
- Forced stop is explicit and counted.
- Actor drain timeout is visible through CLI and metrics.

Observability:

- `hpactor_actor_drain_pending_messages`
- `hpactor_actor_drain_duration_seconds`
- `hpactor_actor_forced_stop_total`

Tests:

- drain completes for finite mailbox
- timeout forces stop
- user messages are DLQ routed under `DropUserMessages`
- protected system messages remain deliverable

### AR-003: Structured Actor Failure Envelope

Priority: `P0`
Status: `Designed`
Release lane: `Foundation`

Design: [Structured Failure Envelope Design](structured-failure-envelope-design.md)

Gap:

Failures across send, ask, RPC, spawn, supervision, remote node loss, and mailbox
overflow do not share one typed envelope. Operators cannot reliably correlate
why a request failed.

Architecture requirement:

Add a shared `FailureEnvelope`:

- `FailureReason` enum shared by all failure paths (route, lifecycle, resource,
  time, policy, transport, dedup, shutdown, reliable messaging, spawn).
- `FailureEnvelope` struct: actor id, sender address, receiver address, message
  id, trace id, reason code, retryable flag, timestamp, subsystem source,
  human-readable detail.
- `FailureSource` enum identifying which subsystem produced the failure.

Runtime contract:

- Every failed delivery or runtime control failure maps to a reason code.
- User-visible errors can carry the envelope or a stable projection of it.
- Logs, traces, metrics, and DLQ records use the same reason vocabulary.

Dependencies:

- Delivery semantics.
- Distributed tracing.
- DLQ.

Acceptance evidence:

- Failure reasons are shared by actor send, RPC, spawn, remote delivery, and
  mailbox admission.
- No new production path returns only an opaque `unknown` error when a precise
  reason is available.

Observability:

- `hpactor_failures_total{reason,subsystem}`
- failure envelope fields in structured logs
- trace span status and attributes from envelope

Tests:

- no route
- actor dead
- mailbox full
- remote unavailable
- timeout
- duplicate suppressed

### AR-004: Actor Quarantine And Circuit Breaker

Priority: `P1`
Status: `Missing`
Release lane: `Cluster`

Gap:

An actor that repeatedly fails, times out, or overloads its mailbox can continue
receiving traffic and destabilize callers.

Architecture requirement:

Add actor-level quarantine and circuit breaker policy:

- failure-rate threshold
- timeout-rate threshold
- mailbox pressure threshold
- cooldown period
- half-open probe behavior
- operator override

Runtime contract:

- Quarantined actors reject or DLQ user messages according to policy.
- System messages for inspection, stop, and supervision still work.
- Circuit breaker state is visible and auditable.

Dependencies:

- Actor lifecycle state machine.
- Failure envelope.
- Mailbox pressure metrics.

Acceptance evidence:

- Actors transition to quarantine after configured threshold.
- Recovery path exists through cooldown, supervisor restart, or operator action.
- Senders receive `RejectedByPolicy` or equivalent typed failure.

Observability:

- `hpactor_actor_circuit_state`
- `hpactor_actor_quarantine_total`
- actor quarantine audit logs

Tests:

- repeated failure opens breaker
- cooldown permits half-open probe
- successful probe closes breaker
- operator override clears quarantine

## 5. Messaging Requirements

### MSG-001: Delivery Result Contract

Priority: `P0`
Status: `Missing`
Release lane: `Foundation`

Gap:

`send()` is fire-and-forget, `deliver_local()` returns `void`, and remote
delivery failures do not consistently flow back into observable typed results.

Architecture requirement:

Define `DeliveryStatus` and `DeliveryResult` for local and remote delivery:

- `Accepted`
- `NoRoute`
- `ActorDead`
- `MailboxFull`
- `Expired`
- `Duplicate`
- `RemoteUnavailable`
- `RejectedByPolicy`
- `SerializationError`
- `TransportError`

Runtime contract:

- Existing `send()` remains source-compatible and ignores the result.
- New `try_send()` and internal `try_deliver_local()` return `DeliveryResult`.
- Remote delivery maps transport and route failures to the same result model.

Dependencies:

- Failure envelope.
- Dead-letter queue.
- Mailbox admission result.

Acceptance evidence:

- Every delivery path produces a result internally.
- `send()` compatibility is preserved.
- `try_send()` exposes actionable results to producers.

Observability:

- `hpactor_delivery_results_total{status}`
- trace attributes for delivery status
- CLI recent delivery failure view

Tests:

- local accepted
- no route
- mailbox full
- actor dead
- expired before enqueue
- remote unavailable

### MSG-002: Message Deadline And TTL Enforcement

Priority: `P0`
Status: `Partial`
Release lane: `Foundation`

Gap:

Priority and deadline parameters appear in delivery signatures, but TTL/deadline
is not consistently enforced before enqueue, before dequeue, and before retry.

Architecture requirement:

Add message deadline metadata and enforce it at:

- sender creation
- local admission
- remote receive
- mailbox dequeue
- retry scheduling
- reliable delivery replay

Runtime contract:

- Expired messages never enter user handlers.
- Expired messages produce `DeliveryStatus::Expired`.
- Expired retained messages go to DLQ when configured.

Dependencies:

- Delivery result contract.
- DLQ.
- Reliable messaging for retry checks.

Acceptance evidence:

- Expired messages are rejected before handler execution.
- Deadline reason is visible in metrics, logs, and traces.
- Remote clock handling is documented as sender deadline plus receiver monotonic
  conversion policy.

Observability:

- `hpactor_messages_expired_total`
- `hpactor_message_deadline_remaining_seconds`

Tests:

- expired before enqueue
- expires while queued
- expires during retry window
- remote receive past deadline

### MSG-003: Reliable Messaging Mode

Priority: `P1`
Status: `Designed`
Release lane: `Durability`

Gap:

RPC has retry behavior, but ordinary actor messages do not have opt-in
at-least-once delivery with ACK/NACK, deduplication, and replay.

Architecture requirement:

Add reliable delivery mode:

- outbound tracker
- ACK/NACK frames
- retry policy
- receiver dedup cache
- optional durable outbox/inbox
- DLQ on retry exhaustion

Runtime contract:

- Reliable delivery is opt-in per message or actor.
- Handler may observe duplicates unless dedup is enabled.
- ACK means admitted to receiver runtime, not fully processed by user handler.

Dependencies:

- Delivery semantics.
- Transport protocol negotiation.
- DLQ.
- Durable delivery store for durable mode.

Acceptance evidence:

- Retry stops after ACK.
- Duplicate message id is suppressed within dedup window.
- Retry exhaustion creates DLQ record.

Observability:

- `hpactor_reliable_outbox_pending`
- `hpactor_reliable_acks_total`
- `hpactor_reliable_nacks_total`
- `hpactor_reliable_retries_total`
- `hpactor_reliable_duplicates_total`

Tests:

- ACK completion
- timeout retry
- duplicate suppression
- NACK retry-after
- retry exhaustion to DLQ

### MSG-004: Batch And Streaming Message Protocol

Priority: `P2`
Status: `Missing`
Release lane: `Maturity`

Gap:

High-throughput workloads and long-running flows require lower per-message
overhead than single-message frames.

Architecture requirement:

Add optional protocols:

- batch frame with multiple typed messages
- stream session with flow-control window
- partial failure handling per message in a batch
- trace propagation per message or batch root

Runtime contract:

- Batch delivery preserves per-target admission result.
- Stream flow control integrates with mailbox and remote endpoint pressure.

Dependencies:

- Delivery result contract.
- Remote overload control.
- Protocol negotiation.

Acceptance evidence:

- Batch frames are negotiated by feature flag.
- Partial batch failure is visible and deterministic.
- Stream senders respect receiver window.

Observability:

- `hpactor_message_batches_total`
- `hpactor_message_batch_size`
- `hpactor_stream_window_bytes`

Tests:

- batch accepted
- partial batch DLQ
- stream backpressure
- incompatible peer rejects batch feature

## 6. Mailbox Requirements

### MBX-001: Bounded Mailbox Admission

Priority: `P0`
Status: `Designed`
Release lane: `Foundation`

Gap:

The mailbox can grow without a hard admission boundary, which risks OOM under a
slow actor or producer storm.

Architecture requirement:

Add bounded mailbox admission by:

- message count
- estimated byte count
- high and low watermarks
- protected system-message reserve
- actor-specific override
- system default

Runtime contract:

- Enqueue returns an admission result.
- Capacity is enforced before memory growth can destabilize the node.
- Capacity can be configured through topology and runtime defaults.

Dependencies:

- Delivery result contract.
- Actor lifecycle.
- Metrics and CLI.

Acceptance evidence:

- Mailbox rejects or applies policy at capacity.
- Per-actor TOML capacity takes effect.
- System default applies when actor override is absent.

Observability:

- `hpactor_mailbox_capacity`
- `hpactor_mailbox_depth`
- `hpactor_mailbox_bytes`
- `hpactor_mailbox_admission_total`

Tests:

- accept under capacity
- reject at capacity
- system message reserve
- per-actor config override

### MBX-002: Overflow Policy Engine

Priority: `P0`
Status: `Designed`
Release lane: `Foundation`

Gap:

Overflow behavior is not policy-driven. Production actors need different
responses depending on message criticality and actor role.

Architecture requirement:

Implement overflow policies:

- `Reject`
- `DropNewest`
- `DropOldest`
- `DeadLetter`
- `OverflowQueue`
- `SignalOnly`
- `NoDropSystem`

Runtime contract:

- Policy decision is deterministic and visible.
- Dropped messages produce accounting and optional DLQ records.
- Protected system messages are not displaced by normal user messages.

Dependencies:

- DLQ.
- Message priority lanes.
- Delivery result contract.

Acceptance evidence:

- Each policy has isolated tests.
- Metrics include policy decision counts.
- CLI shows active policy and counters.

Observability:

- `hpactor_mailbox_overflow_policy_total`
- `hpactor_mailbox_dropped_total`
- `hpactor_mailbox_rejected_total`

Tests:

- each overflow policy
- protected lane behavior
- DLQ handoff
- policy configured by actor

### MBX-003: Backpressure Signal Propagation

Priority: `P0`
Status: `Designed`
Release lane: `Foundation`

Gap:

Producers do not receive pressure before the target actor is overwhelmed.
Remote producers also need endpoint-level signals.

Architecture requirement:

Add local and remote pressure propagation:

- local delivery result
- `BackpressureSignal` system message
- remote pressure control frame
- retry-after hint
- pressure state per actor and endpoint
- signal rate limiting

Runtime contract:

- Producers can slow down before hard rejection.
- Remote pressure does not create control-frame storms.
- Pressure clears when depth falls below low watermark.

Dependencies:

- Bounded mailbox admission.
- Transport control frames.
- Metrics.

Acceptance evidence:

- Pressure enters and exits states based on watermarks.
- Remote pressure signal reaches sender.
- Signals are rate-limited.

Observability:

- `hpactor_backpressure_signals_total`
- `hpactor_actor_pressure_state`
- `hpactor_remote_endpoint_pressure_state`

Tests:

- local pressure signal
- remote pressure frame
- rate limit
- pressure clear

### MBX-004: Remote Outbound Queue Limits

Priority: `P1`
Status: `Missing`
Release lane: `Cluster`

Gap:

Even if actor mailboxes are bounded, outbound transport queues can grow during
slow or disconnected remote peers.

Architecture requirement:

Add endpoint-level outbound limits:

- per-endpoint queued bytes
- per-endpoint queued messages
- priority for control frames
- remote circuit breaker
- spill to reliable outbox only when reliable mode is enabled

Runtime contract:

- Remote endpoint overload cannot exhaust node memory.
- Control frames can still close, drain, or report pressure.
- Best-effort user messages can be rejected or DLQ routed under policy.

Dependencies:

- Delivery result contract.
- Remote overload control frames.
- Reliable messaging for durable spill.

Acceptance evidence:

- Endpoint queue limits reject user messages.
- Control frames remain deliverable.
- Reconnect does not replay rejected best-effort messages.

Observability:

- `hpactor_endpoint_outbound_queue_bytes`
- `hpactor_endpoint_outbound_queue_messages`
- `hpactor_endpoint_send_rejected_total`

Tests:

- slow remote peer
- disconnected endpoint
- control frame under pressure
- recovery after reconnect

## 7. Cluster Requirements

### CLU-001: Cluster Failure Model And Fencing

Priority: `P0`
Status: `Designed`
Release lane: `Cluster`

Gap:

Service discovery can detect members, but the runtime needs a cluster-level
failure model that decides when to route, quarantine, fence, and recover.

Architecture requirement:

Implement node states:

- `Joining`
- `Alive`
- `Suspect`
- `Unreachable`
- `Quarantined`
- `Leaving`
- `Down`
- `Removed`

Add fencing fields:

- cluster id
- node id
- incarnation
- process start id
- certificate fingerprint
- membership epoch

Runtime contract:

- Discovery events feed policy; they do not directly mutate placement.
- Quarantined nodes cannot receive production actor traffic.
- Older incarnations are fenced after restart.

Dependencies:

- Service discovery.
- Security identity.
- Route invalidation.

Acceptance evidence:

- State transitions are legal, logged, and inspectable.
- Duplicate node identity is quarantined.
- Actor location cache purges routes to down or quarantined nodes.

Observability:

- `hpactor_cluster_state_transitions_total`
- `hpactor_cluster_quarantine_total`
- `hpactor_cluster_route_invalidations_total`

Tests:

- suspect to alive
- suspect to down
- duplicate identity
- restart with higher incarnation
- quarantine route rejection

### CLU-002: Sharding And Placement

Priority: `P1`
Status: `Designed`
Release lane: `Cluster`

Gap:

Actors can be remote, but there is no cluster-level placement engine for logical
actor populations or automatic distribution across nodes.

Architecture requirement:

Add:

- logical actor id
- shard id
- shard table with epoch
- shard coordinator
- placement strategies: static, rendezvous hash, load-aware
- shard handoff protocol

Runtime contract:

- Each shard has one active owner.
- Route cache observes shard table epoch.
- Stale owners return shard moved response.

Dependencies:

- Cluster failure model.
- Graceful shutdown.
- Durable state for stateful shard recovery.

Acceptance evidence:

- Logical actor routes to correct shard owner.
- Node loss reassigns shard by policy.
- Handoff drains or rejects in-flight work deterministically.

Observability:

- `hpactor_shards_total`
- `hpactor_shard_moves_total`
- `hpactor_shard_handoff_duration_seconds`
- `hpactor_shard_route_misses_total`

Tests:

- static placement
- rendezvous hash movement count
- node loss reassignment
- stale epoch redirect

### CLU-003: Cluster Singleton And Coordinator Ownership

Priority: `P1`
Status: `Missing`
Release lane: `Cluster`

Gap:

Some system actors must have one active owner in the cluster, such as shard
coordinator, placement coordinator, or scheduled global jobs.

Architecture requirement:

Add cluster singleton support:

- singleton identity
- owner election or external coordinator binding
- fencing token
- standby behavior
- failover policy

Runtime contract:

- At most one active singleton owner can perform mutating actions.
- Ownership changes are audited and observable.
- Split-brain policy controls whether singleton fails closed.

Dependencies:

- Cluster failure model.
- Security identity.
- Optional external coordinator.

Acceptance evidence:

- Singleton owner is visible.
- Conflicting owners are fenced or quarantined.
- Failover occurs only under configured policy.

Observability:

- `hpactor_cluster_singleton_owner`
- `hpactor_cluster_singleton_failover_total`
- singleton ownership audit logs

Tests:

- owner start
- owner loss
- duplicate owner fencing
- graceful owner transfer

### CLU-004: Multi-Zone Placement

Priority: `P2`
Status: `Missing`
Release lane: `Maturity`

Gap:

Production clusters often span zones or racks. Placement should avoid putting
all critical actors in one failure domain.

Architecture requirement:

Add placement metadata:

- region
- zone
- rack
- node weight
- capacity class

Runtime contract:

- Placement strategies can prefer or require diversity.
- Failover can keep traffic within zone or cross zone by policy.

Dependencies:

- Sharding and placement.
- Cluster failure model.

Acceptance evidence:

- Static and load-aware placement can use zone metadata.
- CLI shows failure-domain distribution.

Observability:

- `hpactor_shard_zone_distribution`
- placement decision logs with zone reason

Tests:

- zone-aware placement
- zone failure simulation
- rebalance after zone recovery

## 8. Networking Requirements

### NET-001: Protocol And Feature Negotiation

Priority: `P0`
Status: `Partial`
Release lane: `Cluster`

Gap:

Rolling upgrades and optional features need peers to negotiate protocol version,
frame capabilities, security mode, tracing fields, reliable messaging, and
compression.

Architecture requirement:

Add authenticated handshake metadata:

- protocol min and max
- runtime version
- feature bitmap
- cluster id
- node identity
- compression support
- reliable messaging support
- tracing support

Runtime contract:

- Incompatible peers reject actor traffic before user messages flow.
- Compatible peers downgrade optional features safely.
- Negotiated features are visible per connection.

Dependencies:

- Security identity.
- Rolling upgrade.
- Transport frame versioning.

Acceptance evidence:

- Mixed feature peers negotiate lowest safe feature set.
- Incompatible peers fail with typed reason.
- Connection pool stores negotiated capability.

Observability:

- `hpactor_transport_handshake_total`
- `hpactor_transport_incompatible_peer_total`
- connection capability logs

Tests:

- compatible version
- incompatible version
- feature downgrade
- cluster id mismatch

### NET-002: Frame Hardening And Resource Budgets

Priority: `P0`
Status: `Partial`
Release lane: `Foundation`

Gap:

All network decode paths need strict resource budgets and malformed frame
behavior to prevent memory and CPU abuse.

Architecture requirement:

Enforce:

- max frame size
- max payload size per type
- decode time budget
- max nested protobuf size where applicable
- malformed frame error counters
- connection close policy

Runtime contract:

- Malformed frames never crash the process.
- Oversized frames are rejected before unbounded allocation.
- Repeated bad frames can trip endpoint circuit breaker.

Dependencies:

- Security architecture.
- Transport health scoring.

Acceptance evidence:

- Frame decoder rejects malformed and oversized input.
- Fuzzing does not crash decoder.
- Bad peer is closed or quarantined by policy.

Observability:

- `hpactor_transport_frame_rejected_total`
- `hpactor_transport_decode_errors_total`
- `hpactor_transport_bad_peer_total`

Tests:

- max frame rejection
- malformed length
- corrupted protobuf
- fuzz corpus

### NET-003: Remote Overload Control

Priority: `P0`
Status: `Missing`
Release lane: `Foundation`

Gap:

Remote nodes cannot consistently signal that an actor, endpoint, or node is
overloaded.

Architecture requirement:

Add remote overload frames:

- actor pressure
- endpoint pressure
- node pressure
- retry-after
- non-retryable rejection

Runtime contract:

- Remote senders receive pressure before uncontrolled retries.
- Pressure state decays or clears explicitly.
- Control frames are protected from user-message queue saturation.

Dependencies:

- Mailbox backpressure.
- Endpoint outbound queue limits.
- Protocol negotiation.

Acceptance evidence:

- Pressure frame changes sender behavior.
- Retry-after affects reliable messaging retry schedule.
- Control frames flow under user-message overload.

Observability:

- `hpactor_remote_pressure_frames_total`
- `hpactor_remote_pressure_state`

Tests:

- actor pressure frame
- node pressure frame
- retry-after
- pressure clear

## 9. State And Durability Requirements

### DUR-001: Durable Actor State

Priority: `P1`
Status: `Designed`
Release lane: `Durability`

Gap:

Actor state does not survive process or node failure except through custom user
logic. Hibernation exists, but production durability needs recovery and storage
contracts.

Architecture requirement:

Add durable actor interfaces:

- persistence id
- snapshot serialize
- snapshot restore
- event persist
- event replay
- recovery hook

Runtime contract:

- Durable actors opt in explicitly.
- Recovery completes before user-message handling.
- Store failure policy is explicit.

Dependencies:

- Actor lifecycle.
- Storage adapter.
- Sharding for distributed activation.

Acceptance evidence:

- Snapshot actor recovers after restart.
- Event-sourced actor replays in sequence order.
- Recovery failure quarantines or fails actor by policy.

Observability:

- `hpactor_durable_recovery_total`
- `hpactor_durable_recovery_duration_seconds`
- `hpactor_durable_store_errors_total`

Tests:

- snapshot restore
- event replay
- corrupt snapshot
- store unavailable

### DUR-002: Storage Adapter And Recovery Policy

Priority: `P1`
Status: `Missing`
Release lane: `Durability`

Gap:

There is no abstraction for durable state stores, reliable delivery stores, or
operator-visible recovery policy.

Architecture requirement:

Add storage adapter interfaces:

- durable state store
- reliable delivery store
- metadata store for shard ownership when external coordinator is used

Runtime contract:

- Storage errors return typed failures.
- Backoff and retry are bounded.
- Recovery policy chooses fail, quarantine, or tolerant skip where configured.

Dependencies:

- Durable actor state.
- Reliable messaging.
- Cluster failure model.

Acceptance evidence:

- In-memory test store.
- File-based reference store.
- Error injection path.

Observability:

- `hpactor_store_operations_total`
- `hpactor_store_operation_errors_total`
- `hpactor_store_operation_latency_seconds`

Tests:

- successful write/read
- write failure
- load failure
- recovery retry exhaustion

### DUR-003: Passivation And Reactivation

Priority: `P2`
Status: `Partial`
Release lane: `Maturity`

Gap:

Memory hibernation exists, but distributed actor passivation needs route,
mailbox, durable state, and shard-owner coordination.

Architecture requirement:

Add passivation protocol:

- idle detection
- drain or DLQ policy for queued messages
- final snapshot
- route remains owned by shard
- lazy reactivation
- recovery before message handling

Runtime contract:

- Passivated actor does not consume active actor memory.
- New message triggers activation or returns retryable delivery result.
- Durable state is restored before processing.

Dependencies:

- Durable actor state.
- Actor lifecycle.
- Sharding.

Acceptance evidence:

- Actor can passivate and reactivate.
- Route stays valid through shard owner.
- Passivation does not lose retained state.

Observability:

- `hpactor_actor_passivation_total`
- `hpactor_actor_reactivation_total`
- `hpactor_actor_passivated_count`

Tests:

- idle passivation
- message reactivation
- passivation during shutdown
- passivation recovery failure

## 10. Observability Requirements

### OBS-001: Trace, Log, Metric, And DLQ Correlation

Priority: `P0`
Status: `Designed`
Release lane: `Foundation`

Gap:

Metrics, logs, traces, and failed message records need common correlation keys.

Architecture requirement:

Use these correlation keys consistently:

- trace id
- span id
- message id
- actor id
- node id
- shard id
- delivery status
- failure reason

Runtime contract:

- Trace context propagates through message envelope.
- Logs include trace and message IDs where available.
- DLQ records preserve trace context.
- Metrics use bounded cardinality labels.

Dependencies:

- Distributed tracing.
- Delivery semantics.
- DLQ.
- Logging and metrics.

Acceptance evidence:

- One failed request can be followed through trace, logs, metrics, and DLQ.
- High-cardinality labels are avoided in metric series.

Observability:

- correlation fields in logs and traces
- exemplar support where metrics backend allows it

Tests:

- local trace propagation
- remote trace propagation
- DLQ preserves trace
- log redaction and correlation

### OBS-002: Health And Incident Timeline

Priority: `P0`
Status: `Missing`
Release lane: `Operations`

Gap:

Production orchestration needs health signals, and operators need a timeline of
runtime decisions during incidents.

Architecture requirement:

Add:

- startup health
- readiness health
- liveness health
- incident timeline query model
- event store ring for recent operational decisions

Runtime contract:

- Readiness is false during startup, drain, and critical dependency loss.
- Liveness fails only when the process cannot make progress.
- Incident timeline merges runtime events by correlation key.

Dependencies:

- Operations API.
- Actor lifecycle.
- Cluster failure model.

Acceptance evidence:

- Health reflects drain and dependency status.
- Timeline can show delivery failure, DLQ, actor restart, and cluster event for
  one trace or actor.

Observability:

- `hpactor_health_state`
- `hpactor_incident_events_total`

Tests:

- startup readiness
- drain readiness false
- liveness worker stall
- timeline query by trace id

### OBS-003: Profiling And Cardinality Controls

Priority: `P1`
Status: `Partial`
Release lane: `Operations`

Gap:

Metrics exist, but production systems need profiling hooks and guardrails
against high-cardinality label explosions.

Architecture requirement:

Add:

- metric label allowlist
- actor type label cache budget
- profiling snapshots for scheduler, mailbox wait, actor execution, transport
  queues, and allocator pressure
- top-N views for hot actors and endpoints

Runtime contract:

- Metrics cannot create unbounded series from actor ids by default.
- Profiling snapshots do not read actor state unsafely.

Dependencies:

- Metrics subsystem.
- CLI/Admin API.

Acceptance evidence:

- Cardinality limits are configurable.
- Top-N profiling views work under load.

Observability:

- `hpactor_metrics_series_dropped_total`
- `hpactor_profile_snapshot_total`

Tests:

- label budget exceeded
- top-N actor latency
- scheduler utilization snapshot

## 11. Security Requirements

### SEC-001: Node Identity And mTLS

Priority: `P0`
Status: `Partial`
Release lane: `Operations`

Gap:

TLS exists, but production cluster traffic needs authenticated node identity,
cluster id validation, certificate rotation, and secure defaults.

Architecture requirement:

Add:

- mTLS production profile
- node certificate identity binding
- trust bundle
- cluster id check
- certificate rotation
- plaintext dev mode only by explicit config

Runtime contract:

- Nodes without valid identity cannot join production cluster traffic.
- Certificate mismatch produces typed security failure.
- Rotation can happen without full cluster downtime.

Dependencies:

- Protocol negotiation.
- Cluster failure model.
- Security config.

Acceptance evidence:

- mTLS handshake validates node identity.
- Cluster id mismatch rejects peer.
- Trust bundle reload affects new connections.

Observability:

- `hpactor_security_tls_handshake_total`
- `hpactor_security_auth_failures_total`
- cert reload audit log

Tests:

- valid cert
- invalid cert
- wrong cluster id
- cert reload

### SEC-002: Authorization And Audit

Priority: `P0`
Status: `Missing`
Release lane: `Operations`

Gap:

Remote spawn, RPC, CLI, Admin API, DLQ replay, shutdown, and config reload are
powerful operations and need authorization.

Architecture requirement:

Add:

- static role policy
- resource/action permission model
- enforcement modes: off, permissive, enforce
- audit event schema
- security decision integration with logs and metrics

Runtime contract:

- Mutating operations require permission in enforce mode.
- Denials are auditable and visible.
- Audit records include actor/operator identity, action, resource, decision,
  reason, source endpoint, and trace id.

Dependencies:

- Security identity.
- Admin API.
- Config parser IoC.

Acceptance evidence:

- Unauthorized remote spawn is denied.
- Unauthorized DLQ replay is denied.
- Audit log records allow and deny decisions.

Observability:

- `hpactor_security_authz_denied_total`
- `hpactor_security_audit_events_total`

Tests:

- allowed role
- denied role
- permissive mode logs but allows
- enforce mode denies

### SEC-003: Secret Redaction And Rotation

Priority: `P1`
Status: `Missing`
Release lane: `Operations`

Gap:

Production config and logs must not leak secrets, and certificates or tokens
need rotation paths.

Architecture requirement:

Add:

- secret source abstraction
- sensitive config key redaction
- CLI redaction
- log redaction
- credential reload hooks

Runtime contract:

- Secrets are never printed in normal logs or CLI output.
- Credential reload is audited.
- Invalid new credential leaves old valid credential in place when possible.

Dependencies:

- Dynamic config reload.
- Security config.

Acceptance evidence:

- Redaction tests cover config errors, CLI, and logs.
- Credential reload failure is safe and observable.

Observability:

- `hpactor_security_secret_reload_total`
- secret reload audit log

Tests:

- config redaction
- CLI redaction
- failed reload
- successful reload

## 12. Operations Requirements

### OPS-001: Node Drain And Rolling Upgrade

Priority: `P0`
Status: `Designed`
Release lane: `Operations`

Gap:

Nodes cannot yet drain traffic, leave the cluster, flush telemetry, and stop
within bounded time under a standard protocol.

Architecture requirement:

Add shutdown phases:

- `Running`
- `DrainingIngress`
- `DrainingActors`
- `LeavingCluster`
- `FlushingTelemetry`
- `Stopped`
- `ForcedStop`

Runtime contract:

- Readiness becomes false at drain start.
- New user ingress is rejected or rerouted.
- Actor drain policy executes.
- Cluster advertises leaving.
- Timeout forces stop with accounting.

Dependencies:

- Actor drain policy.
- Cluster failure model.
- Health endpoints.
- DLQ.

Acceptance evidence:

- Node drain completes in bounded time.
- Forced stop is counted.
- Rolling upgrade can run mixed-version compatible nodes.

Observability:

- `hpactor_shutdown_phase`
- `hpactor_shutdown_duration_seconds`
- `hpactor_shutdown_forced_total`

Tests:

- graceful drain
- actor drain timeout
- telemetry flush timeout
- mixed-version handshake

### OPS-002: Authenticated Admin API

Priority: `P1`
Status: `Missing`
Release lane: `Operations`

Gap:

CLI exists, but production automation needs a secure, scriptable Admin API.

Architecture requirement:

Add Admin API resources:

- actors
- mailboxes
- cluster nodes
- shards
- DLQ
- reliable messaging
- config
- shutdown
- security audit

Runtime contract:

- Read-only and mutating actions are separated.
- Mutating actions require authorization.
- Responses are structured and stable for automation.

Dependencies:

- Security authorization.
- Operations health model.
- CLI introspection APIs.

Acceptance evidence:

- Admin API can inspect actor and cluster state.
- Unauthorized mutating command is denied.
- Mutating command emits audit event.

Observability:

- `hpactor_admin_requests_total`
- `hpactor_admin_request_duration_seconds`
- admin audit logs

Tests:

- read-only request
- mutating request allowed
- mutating request denied
- malformed request

### OPS-003: Dynamic Config Validation And Reload

Priority: `P1`
Status: `Designed`
Release lane: `Operations`

Gap:

Production systems need safe config validation, diff, and reload for selected
settings.

Architecture requirement:

Add:

- subsystem-owned parser and validator
- config findings
- config diff
- reload plan
- reload classes: live, drain required, restart required, immutable
- audit event for reload

Runtime contract:

- Reload only applies declared safe settings.
- Unsafe changes are rejected with structured findings.
- Failed reload does not leave partial config applied past commit point.

Dependencies:

- TOML parser IoC.
- Admin API.
- Security authorization.

Acceptance evidence:

- Config validate command returns structured findings.
- Reload applies live log-level change.
- Immutable transport change is rejected at runtime.

Observability:

- `hpactor_config_reload_total`
- `hpactor_config_reload_failure_total`
- `hpactor_config_validation_findings_total`

Tests:

- valid reload
- invalid reload
- restart-required rejection
- audit log

## 13. Config And Developer Experience Requirements

### CFG-001: TOML Parser IoC By Subsystem

Priority: `P0`
Status: `Planned`
Release lane: `Foundation`

Gap:

`parse_file_data` has grown into a subsystem parser for metrics, logging, CLI,
discovery, imports, dispatchers, templates, and actors. New production features
would make it worse.

Architecture requirement:

Add parser IoC:

- `TomlTableView`
- parser interfaces
- registry of parser factories
- static self-registration by parser translation unit
- subsystem-owned validation
- no `toml++` in public HPActor parser interfaces

Runtime contract:

- `TomlParser::parse()` remains stable.
- New subsystem config is added by adding a parser source file.
- `parse_file_data` coordinates loading and parser invocation only.

Dependencies:

- Existing TOML topology tests.
- CMake exception handling for TOML adapter sources.

Acceptance evidence:

- Metrics, logging, CLI, discovery, topology are separate parser files.
- Public headers do not include `toml.hpp`.
- Tests prove static self-registration and duplicate parser rejection.

Observability:

- config parser name in validation findings
- config parse errors include file path and section path

Tests:

- existing TOML parser tests
- parser registry tests
- subsystem config fixture tests

### CFG-002: Typed Protocol And Schema Tooling

Priority: `P1`
Status: `Missing`
Release lane: `Maturity`

Gap:

Large actor systems need better schema compatibility, typed protocol checking,
and generated configuration or message bindings.

Architecture requirement:

Add developer tooling:

- actor protocol schema validation
- protobuf compatibility checks
- generated typed actor registration helpers
- topology config schema export
- binary topology compatibility checks

Runtime contract:

- Schema incompatibilities fail in CI or startup, not during traffic.
- Generated helpers remain optional.

Dependencies:

- TOML parser IoC.
- Protocol negotiation.
- Binary topology schema versioning.

Acceptance evidence:

- Compatibility check catches removed field or incompatible type.
- Generated actor helper compiles in examples.
- Binary topology version mismatch is reported clearly.

Observability:

- startup schema validation logs
- config validation findings

Tests:

- compatible schema
- incompatible schema
- generated helper smoke test
- binary topology version mismatch

## 14. Testing Requirements

### TST-001: Deterministic Fault Injection

Priority: `P0`
Status: `Missing`
Release lane: `Foundation`

Gap:

Reliability cannot be proven without injecting faults into mailbox, network,
scheduler, allocator, storage, and config paths.

Architecture requirement:

Add fault injection framework:

- named fault points
- deterministic seeds
- probability and count-based triggers
- scoped enable/disable
- test-only and opt-in runtime mode

Runtime contract:

- Fault injection is disabled by default.
- Fault behavior is reproducible.
- Fault points emit traceable test events.

Dependencies:

- Test harness.
- Subsystem-specific fault hooks.

Acceptance evidence:

- Tests can inject each critical fault category.
- Failing seed can be replayed.

Observability:

- `hpactor_fault_injections_total`
- fault timeline log in tests

Tests:

- mailbox admission failure
- transport drop
- allocator failure
- scheduler pause
- store write failure

### TST-002: Chaos, Soak, Fuzz, And Compatibility Suite

Priority: `P0`
Status: `Missing`
Release lane: `Maturity`

Gap:

Current tests are unit-heavy. Production confidence needs long-running and
failure-heavy tests.

Architecture requirement:

Add test lanes:

- deterministic fault tests
- multi-process chaos tests
- long-running soak tests
- fuzz tests
- sanitizer matrix
- protocol and config compatibility matrix
- performance regression benchmarks

Runtime contract:

- Fast tests remain fast.
- Nightly and release lanes run heavier reliability tests.
- Failures produce saved seed, logs, metrics, and reproduction command.

Dependencies:

- Fault injection.
- Cluster harness.
- Network proxy or faultable transport.

Acceptance evidence:

- Node kill and partition chaos tests exist.
- Soak test tracks memory and latency trends.
- Fuzz tests reject malformed input without crash.
- Compatibility matrix covers at least one previous version.

Observability:

- reliability test reports
- soak trend artifacts
- benchmark regression output

Tests:

- node kill under load
- partition and heal
- TOML fuzz
- frame fuzz
- rolling upgrade compatibility

## 15. Dependency Graph

Foundation dependency chain:

```text
CFG-001
  -> MSG-001
  -> AR-003
  -> MBX-001
  -> MBX-002
  -> MBX-003
  -> DLQ implementation
  -> OBS-001
  -> OBS-002
  -> OPS-001
```

Cluster dependency chain:

```text
NET-001
  -> SEC-001
  -> CLU-001
  -> CLU-002
  -> CLU-003
  -> CLU-004
```

Durability dependency chain:

```text
MSG-001
  -> MSG-003
  -> DUR-002
  -> DUR-001
  -> DUR-003
```

Testing dependency chain:

```text
TST-001
  -> TST-002
```

## 16. Release Slices

### Slice A: Production Foundation

Scope:

- `CFG-001`
- `MSG-001`
- `MSG-002`
- `AR-001`
- `AR-002`
- `AR-003`
- `MBX-001`
- `MBX-002`
- `MBX-003`
- `OBS-001`
- `OBS-002`
- `OPS-001`

Exit criteria:

- No unbounded mailbox growth.
- Failed delivery is typed and observable.
- DLQ records undeliverable messages.
- Node can drain and stop cleanly.
- Trace, log, metric, and DLQ correlation works for one request.

### Slice B: Safe Cluster Operation

Scope:

- `NET-001`
- `NET-002`
- `NET-003`
- `SEC-001`
- `SEC-002`
- `CLU-001`
- `CLU-003`
- `OPS-002`

Exit criteria:

- Nodes negotiate compatible protocol.
- mTLS identity gates cluster traffic.
- Partitions and duplicate node identity have defined outcomes.
- Admin API is authenticated and audited.

### Slice C: Scalable Placement

Scope:

- `CLU-002`
- `CLU-003`
- `CLU-004`
- `DUR-001`
- `DUR-003`

Exit criteria:

- Logical actors route through shard ownership.
- Shard movement is observable and epoch-based.
- Durable actors can recover before activation.

### Slice D: Reliable Critical Workloads

Scope:

- `MSG-003`
- `DUR-001`
- `DUR-002`
- `DUR-003`
- `AR-004`

Exit criteria:

- Critical messages can use at-least-once delivery.
- Receiver dedup suppresses duplicates.
- Durable actors recover after process restart.
- Actor circuit breakers prevent repeated failure storms.

### Slice E: Platform Maturity

Scope:

- `TST-001`
- `TST-002`
- `CFG-002`
- `MSG-004`
- `OBS-003`
- `SEC-003`

Exit criteria:

- Chaos, soak, fuzz, compatibility, and benchmark lanes exist.
- Schema and protocol compatibility checks exist.
- Streaming and batch protocols have negotiated support.
- Secret redaction and rotation are tested.

