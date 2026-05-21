# Architecture Requirement Backlog for Production HPActor

## 1. Purpose

This backlog turns HPActor's production feature gaps into architecture
requirements. It is organized by subsystem and intended to feed future design
docs and implementation plans.

Priority:

- `P0`: required for safe 24x7 production operation.
- `P1`: required for scalable multi-node production.
- `P2`: important for mature platform operation.

Status:

- `Missing`: no implementation yet.
- `Designed`: architecture doc exists, implementation not complete.
- `Partial`: some implementation exists, production contract incomplete.

## 2. Actor Runtime Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| ACT-001 | P0 | Missing | Define actor lifecycle states for starting, active, draining, stopping, stopped, failed, hibernating, passivated, and recovering. |
| ACT-002 | P0 | Missing | Add graceful actor stop protocol with per-actor drain policy and timeout. |
| ACT-003 | P0 | Designed | Add structured failure envelopes for failed send, failed ask, timeout, delivery rejection, and remote node loss. See [Structured Failure Envelope Design](structured-failure-envelope-design.md). |
| ACT-004 | P1 | Missing | Add actor quarantine for repeatedly failing or unsafe actors. |
| ACT-005 | P1 | Missing | Add circuit breaker policy for actors that fail or time out under load. |
| ACT-006 | P1 | Missing | Add actor-local rate limiting and admission policy. |
| ACT-007 | P1 | Partial | Standardize ask/request timeout policy across local actor, remote actor, RPC, and spawn flows. |
| ACT-008 | P2 | Partial | Extend hibernation into production passivation with recovery and route handling. |

### ACT-001: Actor Lifecycle Contract

Actors need a documented and implemented lifecycle state machine. Today there
are actor states in scheduling and lifecycle hooks, but production features need
a shared contract used by supervision, shutdown, passivation, durable recovery,
CLI, metrics, and routing.

Acceptance:

- Lifecycle state transitions are legal and testable.
- CLI and metrics expose lifecycle state.
- New messages are rejected or accepted according to state.
- Supervision and graceful shutdown use the same state model.

### ACT-002: Graceful Actor Stop

Actors need stop policies that decide what happens to mailbox contents and
in-flight work. This is required for rolling upgrades and controlled shutdown.

Acceptance:

- `Drain`, `DropUserMessages`, `SnapshotAndStop`, and `ImmediateStop` policies
  exist.
- Actor stop timeout cannot block node shutdown indefinitely.
- Dropped messages are dead-lettered with reason.

### ACT-003: Structured Failure Envelopes

Generic errors are insufficient for production. Senders and operators need
typed failures such as no route, mailbox full, actor dead, timeout, duplicate,
and rejected by policy.

Acceptance:

- Failure envelopes include actor id, message id, trace id, reason, retryable
  flag, and timestamp.
- RPC, spawn, actor send, DLQ, and tracing use shared reason codes.

## 3. Messaging Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| MSG-001 | P0 | Missing | Define actor delivery semantics for best effort, observable best effort, at-least-once, and durable at-least-once. |
| MSG-002 | P0 | Missing | Add result-returning local and remote delivery APIs. |
| MSG-003 | P0 | Partial | Enforce message deadline/TTL before enqueue and before handler execution. |
| MSG-004 | P0 | Missing | Route undeliverable messages to DLQ according to policy. |
| MSG-005 | P1 | Missing | Add retry policy and ACK/NACK control frames for reliable messaging. |
| MSG-006 | P1 | Missing | Add receiver deduplication cache keyed by source and message id. |
| MSG-007 | P1 | Missing | Add batch send and batch receive protocol for high-throughput remote flows. |
| MSG-008 | P2 | Missing | Add streaming message protocol for long-running actor data flows. |

### MSG-001: Delivery Semantics

The runtime must define what `send()` means under local delivery, remote
delivery, retry, partitions, mailbox overflow, and actor death.

Acceptance:

- Delivery classes are documented and represented in config/API.
- Every delivery failure maps to `DeliveryStatus`.
- Default `send()` remains source-compatible.

### MSG-005: Reliable Messaging

Critical messages need opt-in at-least-once behavior with retry and
deduplication. This depends on delivery semantics and DLQ.

Acceptance:

- Reliable send stores outbound state until ACK, timeout, or cancellation.
- Receiver suppresses duplicates within configured window.
- Retry exhaustion produces a DLQ record.

## 4. Mailbox Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| MBX-001 | P0 | Designed | Implement bounded mailbox capacity by message count and byte budget. |
| MBX-002 | P0 | Designed | Implement overflow policies: reject, drop newest, drop oldest, dead-letter, overflow queue, and signal-only. |
| MBX-003 | P0 | Designed | Add upstream backpressure signal locally and over remote transport. |
| MBX-004 | P0 | Designed | Implement dead-letter handoff for mailbox overflow policy `DeadLetter`. |
| MBX-005 | P1 | Designed | Add priority-aware mailbox lanes and protected system-message lane. |
| MBX-006 | P1 | Missing | Add remote outbound queue limits and pressure state per endpoint. |
| MBX-007 | P1 | Designed | Expose mailbox capacity, depth, pressure state, drop count, and rejection count through metrics and CLI. |

### MBX-001: Bounded Mailbox Capacity

Unbounded mailboxes can OOM the node. Each actor needs a configurable capacity
and admission result.

Acceptance:

- Per-actor and system default mailbox capacity apply at spawn time.
- Enqueue returns a result.
- Capacity and depth are observable.

### MBX-003: Backpressure Signal

Backpressure must reach producers before failure. Local producers can receive a
result or signal; remote producers need control frames.

Acceptance:

- Pressure state has low/high/critical watermarks.
- Remote senders receive retry-after or slow-down signals.
- Signals are rate-limited to avoid control-plane storms.

## 5. Cluster Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| CLU-001 | P0 | Missing | Define cluster node state model and legal transitions. |
| CLU-002 | P0 | Missing | Add node identity, incarnation, cluster id, and fencing. |
| CLU-003 | P0 | Missing | Define partition policy and split-brain behavior. |
| CLU-004 | P0 | Partial | Invalidate actor routes consistently on node down/quarantine. |
| CLU-005 | P1 | Missing | Add shard coordinator and shard ownership table. |
| CLU-006 | P1 | Missing | Add placement strategy: static, rendezvous hash, and load-aware. |
| CLU-007 | P1 | Missing | Add shard handoff and rebalance protocol. |
| CLU-008 | P1 | Missing | Add cluster singleton support with failure model integration. |
| CLU-009 | P2 | Missing | Add multi-region and zone-aware placement. |

### CLU-001: Cluster Node State Model

Discovery status is not enough. The cluster needs states such as joining,
alive, suspect, unreachable, quarantined, leaving, down, and removed.

Acceptance:

- State transitions are explicit and logged.
- Route invalidation and placement react to state changes through policy.
- CLI exposes current state and transition reason.

### CLU-005: Shard Coordinator

Scalable actor placement requires shard ownership rather than static actor
addresses.

Acceptance:

- Logical actor ids map to shards.
- Each shard has one active owner.
- Owner changes publish an epoch and invalidate stale routes.

## 6. Networking Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| NET-001 | P0 | Partial | Add protocol version and feature negotiation to transport handshake. |
| NET-002 | P0 | Partial | Enforce max frame size, decode budget, and malformed frame rejection everywhere. |
| NET-003 | P0 | Missing | Add remote overload control frames and endpoint pressure state. |
| NET-004 | P0 | Partial | Harden reconnect/backoff policy with jitter, circuit breaker, and health scoring. |
| NET-005 | P1 | Missing | Add frame compression negotiation and byte accounting. |
| NET-006 | P1 | Partial | Add mTLS production profile and cert rotation. |
| NET-007 | P2 | Missing | Add transport-level quality metrics per endpoint and route. |

### NET-001: Protocol Negotiation

Rolling upgrades require peers to know which frame versions and feature flags
are safe.

Acceptance:

- Handshake exchanges min/max protocol version and feature flags.
- Incompatible peers reject actor traffic with clear reason.
- Metrics count incompatible connection attempts.

## 7. State And Durability Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| DUR-001 | P1 | Missing | Add durable actor state interfaces for snapshot and restore. |
| DUR-002 | P1 | Missing | Add event-sourced actor persistence APIs. |
| DUR-003 | P1 | Missing | Add durable state store adapter abstraction. |
| DUR-004 | P1 | Missing | Add recovery lifecycle before actor readiness. |
| DUR-005 | P1 | Missing | Add schema version and event upcasting model. |
| DUR-006 | P2 | Partial | Convert hibernation into passivation with durable recovery option. |
| DUR-007 | P2 | Missing | Add backup, restore, and data retention guidance. |

### DUR-001: Durable Actor State

Selected actors need state recovery after restart or movement.

Acceptance:

- Durable actors opt in explicitly.
- Snapshot load completes before actor handles user messages.
- Recovery failure has a configurable policy.

## 8. Observability Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| OBS-001 | P0 | Designed | Implement distributed tracing and envelope-level trace propagation. |
| OBS-002 | P0 | Partial | Correlate metrics, logs, traces, and DLQ records by trace id and message id. |
| OBS-003 | P0 | Missing | Add health endpoints for startup, liveness, and readiness. |
| OBS-004 | P1 | Missing | Add incident timeline query across actor lifecycle, delivery failures, DLQ, cluster state, and admin actions. |
| OBS-005 | P1 | Partial | Add metric cardinality controls and standard dashboard names. |
| OBS-006 | P1 | Missing | Add profiling hooks for scheduler, actor execution, mailbox wait, allocator pressure, and transport queues. |
| OBS-007 | P2 | Missing | Add alert rule documentation and runbook links. |

### OBS-003: Health Endpoints

Production orchestrators need health signals that represent startup, readiness,
liveness, and drain state.

Acceptance:

- Readiness is false during startup, drain, and critical dependency loss.
- Liveness only fails when the process cannot make progress.
- Health endpoints are protected when security is enabled.

## 9. Security Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| SEC-001 | P0 | Partial | Define node identity and mTLS cluster transport profile. |
| SEC-002 | P0 | Missing | Add authorization for remote spawn, RPC, admin API, CLI, DLQ replay, and shutdown. |
| SEC-003 | P0 | Missing | Protect metrics, health, and admin endpoints. |
| SEC-004 | P0 | Missing | Add audit logging for security-relevant actions. |
| SEC-005 | P1 | Missing | Add certificate and trust bundle rotation. |
| SEC-006 | P1 | Missing | Add secret loading and redaction rules. |
| SEC-007 | P2 | Missing | Add external identity integration hooks. |

### SEC-002: Authorization

Remote spawn, admin actions, and DLQ replay are powerful and must be guarded.

Acceptance:

- Static role policy exists in TOML.
- Mutating admin actions require authorization.
- Authorization failures are audited and observable.

## 10. Operations Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| OPS-001 | P0 | Missing | Add graceful shutdown and node drain protocol. |
| OPS-002 | P0 | Missing | Add readiness/liveness/startup health integration. |
| OPS-003 | P0 | Missing | Add rolling upgrade protocol with compatibility checks. |
| OPS-004 | P1 | Missing | Add authenticated admin API. |
| OPS-005 | P1 | Partial | Add dynamic config validation, diff, and reload for safe settings. |
| OPS-006 | P1 | Missing | Add operational runbook docs for common incidents. |
| OPS-007 | P2 | Missing | Add Kubernetes and systemd deployment guidance. |
| OPS-008 | P2 | Missing | Add autoscaling signal contract. |

### OPS-001: Graceful Shutdown

Nodes must stop receiving work, drain or dead-letter mailboxes, leave the
cluster, flush telemetry, and exit within bounded time.

Acceptance:

- Node shutdown phases are visible.
- Per-actor drain policies exist.
- Forced stop is explicit and counted.

## 11. Config And Developer Experience Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| CFG-001 | P0 | Planned | Refactor TOML parser to IoC static self-registration by subsystem. |
| CFG-002 | P0 | Missing | Add structured config validation report. |
| CFG-003 | P1 | Missing | Add config diff and reload plan. |
| CFG-004 | P1 | Missing | Add schema versioning and deprecated-key warnings. |
| CFG-005 | P1 | Missing | Add typed codegen for actor message protocols. |
| CFG-006 | P2 | Missing | Add compatibility checks for binary topology format. |

### CFG-001: Parser IoC

Each subsystem must own its parser so adding tracing, mailbox, security, or
durability config does not continue growing `parse_file_data`.

Acceptance:

- Parser interfaces do not expose `toml++` in public headers.
- Parser source files self-register with static registrar objects.
- `parse_file_data` coordinates parser invocation only.

## 12. Testing Backlog

| ID | Priority | Status | Requirement |
|----|----------|--------|-------------|
| TST-001 | P0 | Missing | Add deterministic fault injection hooks. |
| TST-002 | P0 | Missing | Add chaos tests for node kill, partition, packet loss, and reconnect. |
| TST-003 | P0 | Missing | Add long-running soak tests for actor send, remote send, mailbox pressure, and memory stability. |
| TST-004 | P1 | Missing | Add fuzz tests for TOML, frames, protobuf decode, HTTP, CLI, and admin API. |
| TST-005 | P1 | Missing | Add sanitizer CI matrix for ASAN, TSAN, and memory debug modes. |
| TST-006 | P1 | Missing | Add protocol and config compatibility test matrix. |
| TST-007 | P2 | Missing | Add performance regression benchmarks and thresholds. |

### TST-001: Fault Injection

Production reliability cannot be proven by happy-path unit tests. The runtime
needs controlled failure points for mailbox, network, scheduler, allocator, and
durable storage.

Acceptance:

- Faults can be enabled deterministically in tests.
- Failures produce expected runtime state and observability.
- Chaos scenarios can be reproduced from a saved seed.

## 13. Suggested Delivery Order

1. `MSG-001`, `MSG-002`, `MBX-001`, `MBX-002`, `MBX-004`
2. `DLQ` implementation from the dead-letter design.
3. `OBS-001`, `OBS-002`, `OBS-003`
4. `OPS-001`, `OPS-002`
5. `CLU-001`, `CLU-002`, `CLU-003`, `CLU-004`
6. `SEC-001`, `SEC-002`, `SEC-004`
7. `CLU-005`, `CLU-006`, `CLU-007`
8. `DUR-001`, `DUR-002`, `DUR-003`, `MSG-005`, `MSG-006`
9. `TST-001`, `TST-002`, `TST-003`, `TST-004`

The first four steps form the recommended "Production Reliability Plane"
milestone.

