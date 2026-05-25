# Mailbox Overflow Policies — Phase 2 Design

**Date:** 2026-05-25
**Status:** Implemented
**Author:** HPActor Team

## Context

Phase 1 of bounded mailbox overflow (PR #96) implemented `DropNewest`, `DropOldest`, and
`DeadLetter` in `MPSCActorMailbox::try_push()`. The remaining five policies —
`RejectNewest`, `DropLowestPriority`, `SpillToOverflowQueue`, `SignalOnly`, and
`BlockWhenAllowed` — fell through to a catch-all default case that rejected with a metric
event but no structured backpressure coordination.

Phase 2 implements three of the five remaining policies (`RejectNewest`, `SignalOnly`,
`SpillToOverflowQueue`), introduces a new `OverflowQueue<T>` auxiliary data structure, and
extends the TOML config surface and observability instrumentation. The two most complex
policies (`DropLowestPriority`, `BlockWhenAllowed`) are explicitly deferred to Phase 3.

## Part 1: OverflowQueue<T>

### Motivation

`SpillToOverflowQueue` requires a secondary bounded queue that absorbs rejected messages
when the main lock-free mailbox is full, then feeds them back on each dequeue. The main
mailbox (`MPSCMailbox<T>`) is lock-free for the hot path; the overflow queue's access
pattern (only touched on overflow and during drain) justifies a simpler mutex-guarded
design.

### Design

```cpp
template <typename T>
class OverflowQueue {
    // Bounded FIFO with configurable max_depth (0 = unlimited).
    // On overflow (push when depth == max_depth), silently evicts oldest.
    // Thread-safe via std::mutex on all operations.
    bool try_push(T&& msg) noexcept;
    bool try_pop(T& out) noexcept;
    OverflowQueueSnapshot snapshot() const noexcept;
    void set_max_depth(uint32_t max_depth) noexcept;
};
```

`OverflowQueueSnapshot` exposes `depth`, `max_depth`, `total_pushed`, `total_popped`,
`total_lost` for observability.

### Rationale

- `std::deque` for FIFO semantics — preserves message ordering within the overflow tier.
- `std::mutex` over lock-free — contention is low (only on main-mailbox-full), and
  `std::deque` operations are already O(1). Lock-free here would add complexity without
  measurable benefit.
- Evict-oldest on overflow capacity — avoids unbounded growth; oldest dropped message
  is the least likely to still be relevant.

## Part 2: Policy Implementations

### RejectNewest

When the mailbox is at capacity and `OverflowPolicy::RejectNewest` is set:
- Increment `total_rejected_` counter.
- Emit `kMailboxRejected` metric event.
- Return `EnqueueResultCode::Rejected` (retryable).

The sender receives a rejection with no `retry_after` — it may retry immediately or
defer to application-level backoff. This is the simplest policy and the default.

### SignalOnly

`SignalOnly` rejects the message but provides a structured backpressure signal:
- Increment `total_rejected_` counter.
- Emit `kMailboxRejected` metric event.
- Return `EnqueueResultCode::Rejected` with `retry_after` set from
  `config_.signal_min_interval_ms` (default 100ms).

On the sender side, `ActorSystem::try_deliver_local()` detects the rejection +
`retry_after > 0` and emits a `BackpressureSignal` with
`BackpressureReason::OverflowPolicy`. This lets the sender's mailbox or proxy throttle
re-sends without losing the message entirely.

The protected system message reserve (`protected_system_messages`) takes precedence over
this policy — system messages (TypeTag < User) that fit within the reserve are still
accepted, preventing deadlock on control messages.

### SpillToOverflowQueue

When the main mailbox is full:
1. Try `overflow_queue_.try_push()` — this always succeeds (evicting oldest overflow
   entry if the overflow queue itself is full).
2. Return `EnqueueResultCode::ReroutedToOverflow` (retryable, not a failure).

On each `try_pop()` from the main mailbox, `drain_overflow()` runs:
1. While overflow queue is non-empty:
   a. Try to reserve main mailbox capacity.
   b. Pop from overflow queue.
   c. Allocate and enqueue the popped message into the main mailbox.
   d. If any step fails, re-queue the message and stop draining.

This design keeps the hot dequeue path lock-free for the common case (overflow queue
empty), and only takes the mutex when draining.

## Part 3: Config Surface

Two new fields in `MailboxConfig`, with TOML parsing:

| Field | Type | Default | TOML key | Purpose |
|-------|------|---------|----------|---------|
| `max_overflow_depth` | `uint32_t` | 0 (unlimited) | `max_overflow_depth` | Max entries in overflow queue |
| `signal_min_interval_ms` | `uint32_t` | 100 | `signal_min_interval_ms` | Retry-after for SignalOnly |

Both are propagated through `ActorSystem::mailbox_config_for_spawn()` and parsed in
`MailboxConfigParser`.

## Part 4: Observability

`MboxSnapshot` gains three overflow-specific fields:
- `overflow_depth` — current overflow queue depth
- `overflow_max_depth` — configured max overflow depth
- `overflow_total_pushed` — cumulative pushes to overflow queue

These are populated from `OverflowQueue::snapshot()` during `MPSCActorMailbox::snapshot()`.

## Deferred to Phase 3

- **`DropLowestPriority`** — requires awareness of mailbox priority levels and
  determining which queued message to evict. Depends on priority-ordered mailbox
  internals not yet exposed.
- **`BlockWhenAllowed`** — requires a blocking wait primitive integrated with the
  scheduler, signaling the blocked producer when capacity frees up. Most invasive
  of the remaining policies.

## Acceptance Criteria

- `RejectNewest` rejects at capacity with `kMailboxRejected` metric.
- `SignalOnly` rejects with `retry_after` and triggers `BackpressureReason::OverflowPolicy`.
- `SignalOnly` still admits system messages within the protected reserve.
- `SpillToOverflowQueue` accepts spills and returns `ReroutedToOverflow`.
- Drain-back on dequeue: messages spilled to overflow are delivered in FIFO order
  when the main mailbox drains.
- Overflow queue itself is bounded; oldest overflow entry is silently evicted on
  overflow.
- TOML `max_overflow_depth` and `signal_min_interval_ms` are parsed and propagated.
- CLI mailbox snapshots include overflow depth and counters.
