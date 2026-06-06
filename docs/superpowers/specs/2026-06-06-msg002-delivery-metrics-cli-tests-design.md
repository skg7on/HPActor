# MSG-002 Delivery Observability & Integration Tests — Design Spec

**Date:** 2026-06-06
**Issue:** [#15](https://github.com/skg7on/HPActor/issues/15) — remaining unchecked items
**Status:** Designed

## Context

Issue #15 (MSG-002) added `DeliveryStatus`, `DeliveryResult`, `TransportSendResult`
types and wired them into `try_send()`, `try_reply()`, `deliver_with_result()`.
The core API surface is complete (8/11 Done-When items). Three items remain:

1. No `kDeliveryResult` metric event or `hpactor_delivery_results_total` counter
2. No CLI `/actor delivery <id>` or `/actor delivery-stats <id>` commands
3. No integration tests covering remote delivery round-trips

## Design

### 1. Metrics: Unified Delivery Counter

Add a single `kDeliveryResult` event type and `hpactor_delivery_results_total`
counter labeled by delivery status.

**MetricEventType addition** (`metrics_event.hpp`):
- `kDeliveryResult = 39` — emitted once per `try_send()`/`try_reply()`/
  `deliver_with_result()` outcome, regardless of success or failure
- `code` field carries the `DeliveryStatus` enum value

**Counter family** (`metrics_aggregator.cpp`):
- Name: `hpactor_delivery_results_total`
- Type: Counter
- Labels: `actor_id`, `actor_type`, `status` (snake_case string from
  `to_string(DeliveryStatus)`)
- Registered in `Aggregator::ensure_families_registered()`
- Handled in `Aggregator::on_event()` switch: resolves actor labels, appends
  status label from `to_string(static_cast<DeliveryStatus>(e.code))`, increments
  counter

**Emission points** (`actor_system.cpp`):
- At the bottom of `try_deliver_local()`, after the `EnqueueResult` is finalized
  (captures both accepted and rejected outcomes). Emit a `kDeliveryResult` event
  with `code` = the `DeliveryStatus` mapped from the `EnqueueResult`.
- In `deliver_with_result()`, after `from_enqueue()` mapping.
- The existing `kDeliveryFailure`/`kDeliveryDuplicate`/`kDeliveryExpired` events
  continue to fire for backward compatibility; the aggregator wires them to also
  increment `hpactor_delivery_results_total`.

**Why a single counter with status label:** Follows OpenMetrics best practice.
Operators slice with `sum by(status) (hpactor_delivery_results_total)` in
Prometheus. Avoids metric cardinality explosion from per-status counter names.

### 2. CLI: Delivery Inspection Commands

Follow the existing `/actor <id>/<subcommand>` pattern using `InspectStateRequest`
→ `InspectStateReply`.

**Proto changes** (`cli_messages.proto`):
Add to `MailboxSnapshot`:
```protobuf
uint64 delivery_accepted_total = 23;
uint64 delivery_rejected_total = 24;
uint64 delivery_failed_total = 25;
uint64 delivery_retryable_total = 26;
```

**Mailbox counters** (`mailbox_policy.hpp` or `mpsc_actor_mailbox.hpp`):
Add four `std::atomic<uint64_t>` counters to the mailbox class:
- `delivery_accepted_total_` — Accepted + AcceptedWithPressure
- `delivery_rejected_total_` — all non-accepted results
- `delivery_failed_total_` — non-retryable failures (Expired, Duplicate,
  RejectedByPolicy, SerializationError, ShuttingDown)
- `delivery_retryable_total_` — retryable failures (NoRoute, ActorDead,
  MailboxFull, RemoteUnavailable, TransportError)

These are incremented in the mailbox's admission boundary where
`EnqueueResult` is finalized, alongside the existing `total_rejected_` and
`total_dropped_` counters.

**MailboxSnapshot population:** The inspect handler reads these counters and
populates the proto fields, same pattern as `total_rejected`/`total_dropped`.

**CLI commands** (`actor_commands.cpp`):
- `ActorDeliveryCommand` — path `actor/<id>/delivery`, order 285
  - Displays: accepted, rejected, failed (non-retryable), retryable counts
- `ActorDeliveryStatsCommand` — path `actor/<id>/delivery-stats`, order 286
  - Displays same counters plus derived ratios (accept rate %, retry rate %)
- Both registered via `CommandRegistration<T>`

### 3. Integration Tests: Remote Delivery Round-Trips

Two test files, two approaches:

#### 3a. MockTransport Tests

**New file:** `tests/integration/actor/test_remote_delivery_result.cpp`

A configurable `MockTransport` that returns a preset `TransportSendResult`.
Tests verify full `DeliveryResult` propagation through the actor proxy path:

| TransportSendResult | Expected DeliveryStatus |
|---------------------|------------------------|
| Sent | Accepted |
| NotConnected | RemoteUnavailable |
| QueueFull | RemoteUnavailable |
| CircuitOpen | RemoteUnavailable |
| EncodeError | SerializationError |
| ShuttingDown | ShuttingDown |
| WriteError | TransportError |

Also test: `ActorContext::try_reply()` returns `DeliveryResult`, and
`DeliveryResult::from_transport()` preserves `target` and `message_id`.

Uses `scheduler_threads = 0` for deterministic execution.

#### 3b. Loopback Integration Test

**Added to:** `tests/integration/actor/test_delivery_semantics.cpp`

Single test that:
- Starts two `ActorSystem` instances on loopback (`127.0.0.1:0`)
- Spawns an actor on system B
- Sends a message from system A to system B's actor via `try_send()`
- Verifies `DeliveryResult::Accepted`
- Terminates the target actor on system B
- Sends again, verifies `DeliveryStatus::ActorDead` or `NoRoute`

Uses `scheduler_threads = 1` with polling for the remote path (needs the event
loop to process TCP).

## Files Changed

| File | Change |
|------|--------|
| `include/hpactor/metrics/metrics_event.hpp` | Add `kDeliveryResult = 39` |
| `include/hpactor/metrics/metrics_aggregator.hpp` | Add `delivery_results_family_` |
| `src/metrics/metrics_aggregator.cpp` | Register family, handle event |
| `src/actor/actor_system.cpp` | Emit `kDeliveryResult` from `try_deliver_local` and `deliver_with_result` |
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | Add delivery tracking counters |
| `src/mailbox/mpsc_actor_mailbox.cpp` | Increment counters in admission path |
| `protos/hpactor/cli_messages.proto` | Add delivery fields to `MailboxSnapshot` |
| `src/cli/commands/actor_commands.cpp` | Add `ActorDeliveryCommand`, `ActorDeliveryStatsCommand` |
| `tests/integration/actor/test_remote_delivery_result.cpp` | **New** — MockTransport round-trip tests |
| `tests/integration/actor/test_delivery_semantics.cpp` | Add loopback remote test |

## Constraints

- No exceptions, no RTTI, no dynamic_cast
- Counters use `std::memory_order_relaxed` (consistent with existing counters)
- CLI commands use `InspectStateRequest` pattern (not direct metrics access)
- Loopback test uses OS-assigned ports (`127.0.0.1:0`) to avoid port conflicts
- Loopback test polls with 5s timeout (consistent with CI timeout policy)
