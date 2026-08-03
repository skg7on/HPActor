# 3. Architecture

**HPActor is a C++20 event-based actor framework. Preserve actor boundaries;
avoid shared mutable state between actors.**

## Reliability Plane Alignment

Production-facing features MUST align with three planes:

| Plane | Concerns |
|-------|----------|
| **Data plane** | Delivery semantics, mailbox admission, DLQ, reliable messaging, tracing, actor lifecycle |
| **Control plane** | Cluster failure model, node identity, sharding, placement, rebalancing, graceful shutdown, rolling upgrades |
| **Operations plane** | Health endpoints, admin API, security, audit, config reload, incident timelines, chaos/soak testing |

## Design Flow

1. Start from the relevant document in `docs/architecture/production/`.
2. Capture runtime contracts, failure semantics, observability, and operations
   surface before writing implementation.
3. Include the operations surface in every production-facing design: metrics,
   CLI/admin visibility, health/readiness, audit/trace correlation, and
   runbook impact.

## API Compatibility

- Preserve source-compatible defaults for existing actor APIs.
- Production-grade behavior (delivery results, bounded mailboxes, reliable
  messaging, tracing, security, durability) MUST be opt-in or safely defaulted.
