# Production Reliability Plane Architecture

## 1. Executive Summary

HPActor already has a strong local runtime and a growing distributed runtime:
actors, scheduling, memory management, remote messaging, RPC, discovery,
metrics, logging, CLI introspection, topology config, and architecture designs
for tracing and mailbox backpressure.

The next evolution is the production reliability plane. Its purpose is to make
HPActor safe for 24x7 operation under overload, node loss, rolling upgrades,
network partitions, hostile clients, and long-running operational drift.

This reliability plane is not one feature. It is a set of cross-cutting runtime
contracts that sit above the actor data path and below user applications:

- Message delivery semantics.
- Mailbox admission, backpressure, and dead-letter handling.
- Cluster failure detection and partition behavior.
- Placement, sharding, and rebalancing.
- Graceful shutdown and rolling upgrades.
- Security, identity, authorization, and audit.
- Health, operations, observability, and incident diagnostics.
- Durability options for critical actors and messages.
- Chaos, soak, compatibility, and fuzz testing.

The goal is to keep the actor API pleasant while making failure behavior
explicit enough that operators can reason about production incidents.

## 2. Current Strengths

HPActor has several strong foundations:

- `ActorSystem` is the central runtime boundary for spawn, registry, local
  delivery, topology loading, and cluster integration.
- `ActorContext` and `ActorRef` give location-transparent send/reply APIs.
- Unified `TypedMessage` and remote frames provide one message carrier for local
  and remote paths.
- `MPSCActorMailbox` is a high-throughput per-actor queue.
- The scheduler already supports work stealing, priorities, deadlines, timers,
  coroutine support, dedicated actors, and worker-level execution.
- The memory subsystem has allocator telemetry, actor ownership, poisoning,
  hibernation, compaction, and typed memory regions.
- Metrics, logging, CLI, and TOML topology already provide production-facing
  observability and configuration entry points.
- Service discovery has pluggable backends and SWIM gossip membership.

These make HPActor a good base for a production distributed actor platform.

## 3. Production Reliability Planes

### 3.1 Data Plane

The data plane moves messages and executes actors.

Responsibilities:

- Local and remote message delivery.
- Mailbox admission and scheduling wakeups.
- Serialization, frame validation, and transport flow control.
- Retry, timeout, deduplication, and delivery result handling.
- Actor execution, lifecycle, supervision, and mailbox draining.

Primary docs:

- [Structured Failure Envelope](structured-failure-envelope-design.md)
- [Actor Delivery Semantics](actor-delivery-semantics-design.md)
- [Dead-Letter Queue](dead-letter-queue-design.md)
- [Reliable Messaging](reliable-messaging-design.md)
- Existing [Mailbox Management and Backpressure](../actor/mailbox-management-backpressure-design.md)
- Existing [Distributed Tracing](../actor/distributed-tracing-design.md)

### 3.2 Control Plane

The control plane decides where actors live and how cluster state changes.

Responsibilities:

- Node membership and failure model.
- Actor placement and sharding.
- Shard movement and rebalancing.
- Cluster singleton and coordinator ownership.
- Quarantine, fencing, and split-brain policy.
- Rolling upgrade compatibility.

Primary docs:

- [Cluster Failure Model](cluster-failure-model-design.md)
- [Cluster Sharding and Placement](cluster-sharding-placement-design.md)
- [Graceful Shutdown and Rolling Upgrade](graceful-shutdown-rolling-upgrade-design.md)
- Existing [Service Discovery Architecture](../net/service-discovery-architecture.md)

### 3.3 Operations Plane

The operations plane lets humans and automation run the system safely.

Responsibilities:

- Health, readiness, liveness, and startup gates.
- Admin API and CLI control.
- Metrics, logs, traces, and incident timelines.
- Security posture and audit records.
- Config validation and dynamic reload.
- Chaos, soak, fuzz, and compatibility testing.

Primary docs:

- [Operations and SRE](operations-sre-design.md)
- [Security Architecture](security-architecture-design.md)
- [Dynamic Config and Parser IoC](dynamic-config-parser-ioc-design.md)
- [Chaos and Reliability Testing](chaos-reliability-testing-design.md)

## 4. Reliability Contract Themes

### 4.1 Explicit Failure Semantics

Production systems fail constantly. HPActor should expose failures as structured
state and delivery results instead of silent drops or opaque `unknown` errors.

Examples:

- `DeliveryResult::NoRoute`
- `DeliveryResult::MailboxFull`
- `DeliveryResult::ActorDead`
- `DeliveryResult::NodeUnavailable`
- `DeliveryResult::TimedOut`
- `DeliveryResult::Duplicate`
- `DeliveryResult::RejectedByPolicy`

### 4.2 Bounded Resources

Every hot path needs a resource boundary:

- Mailbox message count and byte count.
- Dead-letter queue capacity and retention.
- Retry windows.
- Trace and metric ring buffers.
- Remote outbound queues.
- Per-node and per-actor memory budgets.
- Admin API request concurrency.

Bounded resources must report drops, rejections, and policy decisions.

### 4.3 Observable Control Decisions

Operators need to see why the runtime made a decision:

- Why an actor moved.
- Why a message was dead-lettered.
- Why a node was quarantined.
- Why a shard owner changed.
- Why a graceful shutdown refused to complete.
- Why a config reload was rejected.

Each control-plane decision should emit a structured log, metric event, and CLI
inspectable state.

### 4.4 Optional Durability

HPActor should not force every message and actor into durable storage. The
runtime should offer durability as an opt-in mode for workloads that need it:

- Best-effort actors and messages remain fast.
- Critical actor state can use snapshots or event sourcing.
- Critical messages can use durable outbox/inbox.
- Durable features have explicit recovery, replay, and retention contracts.

## 5. Evolution Roadmap

### Milestone 1: Reliability Foundation

- Actor delivery semantics.
- Mailbox backpressure implementation.
- Dead-letter queue.
- Distributed tracing implementation.
- Health/readiness/liveness.
- Graceful shutdown and drain.

### Milestone 2: Cluster Control

- Cluster failure model.
- Quarantine and fencing.
- Shard coordinator.
- Placement strategy.
- Rebalance protocol.
- Rolling upgrade compatibility.

### Milestone 3: Production Operations

- mTLS and node identity.
- Admin API with authorization.
- Dynamic config reload.
- SLO dashboards and alert rules.
- Incident timeline correlation.
- Chaos and soak test suite.

### Milestone 4: Optional Durability

- Durable actor state.
- Reliable messaging.
- Replay tools.
- Snapshot and event schema evolution.
- Storage adapter abstraction.

## 6. Design Principles

1. Keep the default actor API simple.
2. Make overload explicit and observable.
3. Make cluster state monotonic where possible.
4. Prefer bounded queues over hidden memory growth.
5. Keep durability opt-in and policy-driven.
6. Separate data-plane performance from control-plane safety.
7. Support static topology first, then dynamic control.
8. Build operational controls before adding complex automation.
9. Use tests that inject realistic faults, not only unit tests.
10. Treat documentation, metrics, and CLI state as part of the feature.

## 7. Requirement Backlog

The detailed backlog for this production evolution is maintained in:

- [Architecture Requirement Backlog](architecture-requirement-backlog.md)
- [Feature Gap Refined Architecture Requirement Backlog](feature-gap-refined-requirement-backlog.md)
