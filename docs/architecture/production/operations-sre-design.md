# Operations and SRE Architecture Design

## 1. Executive Summary

HPActor needs an operations surface that lets humans and automation run a
cluster safely. Metrics, logs, CLI, and tracing are foundations; production
operation also needs health endpoints, admin APIs, dashboards, alerts, runbook
signals, incident timelines, and safe control commands.

This design defines the operations plane for a 24x7 actor system.

## 2. Goals

1. Provide startup, liveness, and readiness health signals.
2. Expose safe admin APIs for cluster and actor operations.
3. Standardize metrics, logs, traces, and event correlation.
4. Define SLO-oriented dashboards and alert signals.
5. Support incident investigation from one actor/message/trace.
6. Provide operational runbook hooks for drain, quarantine, replay, and config.

## 3. Non-Goals

- Implementing a hosted UI in the runtime.
- Replacing Prometheus, OpenTelemetry, or log backends.
- Letting admin APIs bypass security policy.

## 4. Health Model

Health endpoints:

- `/health/live`: process event loop and critical threads are making progress.
- `/health/ready`: node can receive production traffic.
- `/health/startup`: node completed initialization and topology loading.

Readiness dependencies:

- topology loaded
- system actors active
- service discovery started
- transport listening if network enabled
- durable stores reachable for durable actors
- shard ownership active if sharding enabled
- not draining

## 5. Admin API

The admin API complements CLI and should be scriptable.

Resources:

- `/admin/actors`
- `/admin/cluster/nodes`
- `/admin/cluster/shards`
- `/admin/mailboxes`
- `/admin/dlq`
- `/admin/reliable`
- `/admin/config`
- `/admin/shutdown`
- `/admin/security/audit`

Each mutating endpoint requires authorization and emits audit logs.

## 6. Incident Timeline

An incident timeline merges:

- trace spans
- delivery failures
- DLQ records
- actor lifecycle events
- mailbox pressure events
- cluster membership transitions
- security decisions
- admin actions

Primary correlation keys:

- trace id
- message id
- actor id
- node id
- shard id
- request id

## 7. SLO Signals

Recommended platform SLOs:

- Actor message admission success rate.
- Actor message processing latency.
- Mailbox saturation duration.
- Remote delivery success rate.
- Cluster membership convergence time.
- Shard handoff duration.
- DLQ growth rate.
- Shutdown drain completion rate.

## 8. Alert Classes

Page-worthy:

- node liveness false
- sustained mailbox full for protected actor
- DLQ growth above threshold
- shard coordinator unavailable
- durable store unavailable
- security authorization spike on admin APIs
- cluster partition/quarantine

Ticket-worthy:

- high retry rate
- high trace span drop count
- config reload failures
- slow graceful shutdown
- actor restart storm

## 9. Profiling And Diagnostics

Diagnostics hooks:

- scheduler worker utilization
- actor processing latency histograms
- mailbox wait time
- allocator pressure by region
- transport send/receive queue depth
- connection health
- top actors by CPU approximation and mailbox depth

The CLI and admin API should expose snapshots without reading actor memory
unsafely.

## 10. Acceptance Criteria

- Health endpoints reflect startup, readiness, liveness, and drain state.
- Admin API covers read-only inspection and controlled mutating actions.
- All mutating admin actions are audited.
- Metrics and logs can be correlated with trace id and message id.
- Operators can reconstruct a basic incident timeline for one failed request.
- Alert signal names and thresholds are documented.

