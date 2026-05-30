# MBX-004: Dead-Letter Handoff for Mailbox Overflow Policy DeadLetter — Design Spec

**Date:** 2026-05-30
**Issue:** [#25](https://github.com/skg7on/HPActor/issues/25)
**Subsystem:** Mailbox
**Priority:** P0
**Status:** Design

## 1. Problem Statement

The `DeadLetterQueue` (`include/hpactor/mailbox/dead_letter_queue.hpp`) and its
integration points already exist. The `OverflowPolicy::DeadLetter` enum value
is wired through the overflow handler factory, and `emit_rejection_observability()`
in `actor_system.cpp` checks for it. Superficially, the feature appears done.

However, the actual _handoff_ — the act of moving a message from a full mailbox
into the DLQ with enough fidelity to be useful — has five concrete gaps:

| # | Gap | Impact |
|---|-----|--------|
| G1 | **Payload is silently dropped.** `emit_rejection_observability()` populates metadata but never copies `msg.payload()` into `DeadLetterRecord::payload_sample`. Two sibling paths (`reject_missing_actor`, `try_reject_expired`) already do this correctly. | DLQ records from overflow are unreplayable — they contain reason/sender/target but not the message body. |
| G2 | **No CLI `/dlq` commands.** The design doc (`dead-letter-queue-design.md` §7) specifies `/dlq list`, `/dlq show`, `/dlq replay`, and `/dlq export`. None are registered in `src/cli/commands/`. | Operators cannot inspect, replay, or export DLQ records at runtime. |
| G3 | **`DeadLetterHandler::handle()` has no DLQ access.** The mailbox overflow path returns `ReroutedToDeadLetter` as a result code, but the handler itself only increments a counter and emits a metric. The `OverflowContext` carries no `DeadLetterQueue*` or `IDeadLetterSink*`. | Any code path that pushes to a mailbox directly (bypassing `try_deliver_local`) will produce `ReroutedToDeadLetter` but never actually enqueue a DLQ record. |
| G4 | **`MailboxConfig::enable_dead_letters` is write-only.** Declared with default `true` at `mailbox_policy.hpp:70`, but never read by any production code. | Per-mailbox opt-out is impossible despite the field existing. |
| G5 | **Trace context is not preserved.** `emit_rejection_observability()` does not populate `trace_id_hi`, `trace_id_lo`, or `span_id` on the `DeadLetterRecord`, and does not set `timestamp_ns`. | DLQ records are uncorrelated with distributed traces, making root-cause analysis harder. |

These gaps mean that while the plumbing exists, the feature does not deliver on
the design contract: _messages that overflow into the DLQ must be inspectable,
replayable, and traceable._

## 2. Scope

This spec covers the five gaps above. It does **not** cover:

- Durable DLQ storage (file/object-store sink) — future backlog.
- Cluster-wide DLQ fan-out queries — future backlog.
- Auto-replay or scheduled replay — future backlog.
- Changes to the `DeadLetterQueue` data structure itself (bounded deque is adequate).

## 3. Design

### 3.1. Payload Handoff (G1)

**Root cause:** `emit_rejection_observability()` receives `MailboxEnvelopeMeta` and
`EnqueueResult` but not the `TypedMessage` itself.

**Fix:** Add a `const TypedMessage& msg` parameter to `emit_rejection_observability()`
and copy `msg.payload()` into `dl.payload_sample`, matching the pattern already used
by `reject_missing_actor()` (line 613) and `try_reject_expired()` (line 717).

Also populate `dl.timestamp_ns` with `steady_clock` for all three DLQ paths
(`emit_rejection_observability`, `reject_missing_actor`, `try_reject_expired`) so
records carry creation time consistently.

**Contract:**

```
OverflowPolicy::DeadLetter + full mailbox
  → DeadLetterHandler returns ReroutedToDeadLetter
  → emit_rejection_observability() constructs DeadLetterRecord with:
      - Full metadata (sender, target, type_tag, message_id, flags, priority)
      - Payload sample (truncated to DeadLetterConfig::max_payload_sample_bytes)
      - Timestamp (steady_clock::now())
      - Mailbox depth/capacity at time of rejection
  → DeadLetterQueue::try_push() enforces capacity and payload limits
```

### 3.2. CLI `/dlq` Commands (G2)

Register a `DLQCommands` class in `src/cli/commands/` that adds four commands to
the `CommandNode` tree:

#### `/dlq list`

```
Syntax:  /dlq list [--limit N] [--reason X] [--source Y]
Default: limit=50, no filter
```

Takes a snapshot of the `DeadLetterQueue` and renders records as a paged table:

```
  #   Reason              Source             Target          TypeTag   Age
  0   OverflowPolicy      MailboxAdmission   actor:42        0x0010    12s
  1   ActorNotFound       LocalDelivery      actor:7         0x0003    58s
  ...
```

Columns: index, reason, source, target actor id, type tag hex, age (now - timestamp_ns).

#### `/dlq show <index>`

Shows the full `DeadLetterRecord` at the given index: all metadata fields, payload
hex dump (first 128 bytes), and a structured failure envelope summary.

#### `/dlq replay <index>`

Pops the record at `index` from the DLQ and re-delivers the payload to the original
`target` actor via `ActorSystem::deliver_local()`. Pre-conditions:

- Record must have a non-empty `payload_sample` (replay is impossible without payload).
- Target actor must exist and be in a message-accepting state.
- Emits a `replayed` metric event on success.
- Returns an error message to the CLI on failure (actor gone, no payload, etc.).

Replay is a deliberate operator action — there is no automatic replay.

#### `/dlq export [--format json|text] [--limit N]`

Exports DLQ records in the requested format. JSON format outputs a JSON array of
record objects. Text format outputs one line per record (grep-friendly).

#### Access path

`CliActor` already has access to `ActorSystem` via the `InspectState` mechanism.
`DLQCommands` will follow the same pattern: it receives an `ActorSystem*` reference
during construction and calls `ActorSystem::dead_letter_queue()` to access the DLQ.

A new public accessor is added to `ActorSystem`:

```cpp
mailbox::DeadLetterQueue* dead_letter_queue() noexcept { return dead_letters_.get(); }
```

The `DeadLetterQueue` API needs two additions for CLI access (currently only
supports FIFO `try_pop`):

```cpp
// Return a copy of all records for list/show/export.
std::vector<DeadLetterRecord> snapshot_records() const;

// Remove and return the record at the given index (0-based).
// Returns false if index is out of bounds.
bool try_pop_at(size_t index, DeadLetterRecord& out) noexcept;
```

### 3.3. OverflowContext DLQ Access (G3)

**Problem:** `DeadLetterHandler::handle()` has no way to push to the DLQ. Anyone
calling `mailbox->try_push()` directly (not through `try_deliver_local`) gets
`ReroutedToDeadLetter` but no DLQ record.

**Decision: Keep the two-tier split, but document the contract.**

The mailbox layer is intentionally DLQ-unaware — it operates on the hot path and
should not acquire a mutex (the DLQ is mutex-protected) or allocate DLQ records.
The system layer (`try_deliver_local` / `emit_rejection_observability`) is the
single chokepoint that translates `ReroutedToDeadLetter` into a DLQ push.

**What changes:**

1. Add `DeadLetterQueue*` to `OverflowContext` so handlers _could_ use it, but
   `DeadLetterHandler` remains lightweight (counter + metric only).
2. Add a `DeadLetterQueue* dlq` pointer field to `OverflowContext`, defaulting to
   `nullptr`. This makes the capability available for future handler implementations
   without changing the current handler's behavior.
3. Document in the `IOverflowHandler` interface contract that handlers returning
   `ReroutedToDeadLetter` MUST have their result observed by a caller that pushes
   to the DLQ, or the message is lost.

### 3.4. Config Gating (G4)

**`MailboxConfig::enable_dead_letters`:** This field has no effect. Two options:

- **Option A:** Remove it (it's dead code).
- **Option B:** Wire it as a per-mailbox gate.

**Recommendation: Option A — remove it.** The system-level `DeadLetterConfig::enabled`
already gates the DLQ. Per-mailbox opt-out adds complexity with no clear use case: if
a mailbox uses `OverflowPolicy::DeadLetter`, the operator clearly wants DLQ behavior.
If they don't, they should pick a different overflow policy.

Remove `enable_dead_letters` from `MailboxConfig` and update the one unit test
(`test_mailbox_policy.cpp:34`) that references it.

**`DeadLetterConfig::enabled`:** Already checked inside `DeadLetterQueue::try_push()`.
Add an early-return guard at the `emit_rejection_observability()` call site so we
don't construct a `DeadLetterRecord` only to have `try_push()` discard it:

```cpp
// In emit_rejection_observability():
if (dlq && dlq->config().enabled && overflow_policy == OverflowPolicy::DeadLetter) {
    // ... build and push record
}
```

Add a `config()` accessor to `DeadLetterQueue`:

```cpp
const DeadLetterConfig& config() const noexcept { return config_; }
```

### 3.5. Trace Context Preservation (G5)

Populate the trace fields on `DeadLetterRecord` in all three DLQ paths:

```cpp
dl.trace_id_hi = msg.trace_context().trace_id_hi;
dl.trace_id_lo = msg.trace_context().trace_id_lo;
dl.span_id = msg.trace_context().span_id;
dl.timestamp_ns = /* steady_clock::now() in ns */;
```

`emit_rejection_observability` needs access to the `TypedMessage` for this (which
it already gets from the G1 fix). `reject_missing_actor` and `try_reject_expired`
already have the message — add timestamp and trace fields there too.

## 4. API Changes

### 4.1. `DeadLetterQueue` — new methods

```cpp
class DeadLetterQueue {
  public:
    // Existing API unchanged.
    bool try_push(DeadLetterRecord&& record) noexcept;
    bool try_pop(DeadLetterRecord& out) noexcept;
    DeadLetterQueueSnapshot snapshot() const noexcept;

    // NEW: config accessor for call-site gating.
    const DeadLetterConfig& config() const noexcept { return config_; }

    // NEW: snapshot all records for CLI list/show/export.
    std::vector<DeadLetterRecord> snapshot_records() const;

    // NEW: indexed remove for CLI replay.
    bool try_pop_at(size_t index, DeadLetterRecord& out) noexcept;
};
```

### 4.2. `ActorSystem` — new accessor

```cpp
class ActorSystem {
  public:
    // NEW: expose DLQ for CLI and other subsystems.
    mailbox::DeadLetterQueue* dead_letter_queue() noexcept;
};
```

### 4.3. `OverflowContext` — new field

```cpp
template <typename T> struct OverflowContext {
    // ... existing fields unchanged ...
    mailbox::DeadLetterQueue* dlq = nullptr;  // NEW: available for handlers
};
```

### 4.4. `MailboxConfig` — removed field

Remove `bool enable_dead_letters = true;` from `MailboxConfig`.

### 4.5. `emit_rejection_observability` — new parameter

```cpp
void emit_rejection_observability(
    mailbox::DeadLetterQueue* dlq,
    MetricBuf* metrics,
    EndPoint endpoint,
    ActorId target,
    const TypedMessage& msg,           // NEW: was missing
    const mailbox::MailboxEnvelopeMeta& meta,
    const mailbox::EnqueueResult& result,
    const mailbox::DeliveryOptions& options,
    mailbox::OverflowPolicy overflow_policy);
```

## 5. CLI Command Registration

New file: `src/cli/commands/dlq_commands.hpp` and `src/cli/commands/dlq_commands.cpp`

Commands are registered in the CLI's `CommandNode` tree during `CliActor`
initialization, following the existing pattern used by `actor_commands.cpp`,
`system_commands.cpp`, `failure_commands.cpp`, and `fault_commands.cpp`.

## 6. Observability

### 6.1. Metrics (already wired)

- `kMailboxDeadLetter` — emitted by `DeadLetterHandler::handle()` on overflow.
- `kDeliveryFailure` — emitted by `emit_rejection_observability()` with
  `FailureReason::RejectedByPolicy`.

New metric event for replay:

- `kDeadLetterReplayed` — emitted on successful `/dlq replay`.

### 6.2. Logs (already wired)

- `HPACTOR_LOG_WARNING` with `delivery_failure` in `emit_rejection_observability()`.

### 6.3. CLI (new)

- `/dlq list`, `/dlq show`, `/dlq replay`, `/dlq export` (see §3.2).

## 7. Test Plan

### 7.1. Unit tests

| Test | What it verifies |
|------|-----------------|
| `DeadLetterHandoffPayloadPreserved` | DLQ record from overflow contains `payload_sample` matching the message |
| `DeadLetterHandoffTraceContext` | DLQ record from overflow has `trace_id_hi/lo` and `span_id` populated |
| `DeadLetterHandoffTimestamp` | DLQ record has non-zero `timestamp_ns` |
| `DeadLetterConfigGating` | When `DeadLetterConfig::enabled = false`, `emit_rejection_observability` does not push |
| `DeadLetterHandlerDlqPointer` | `OverflowContext` correctly passes through `dlq` pointer |
| `DeadLetterQueueSnapshotRecords` | `snapshot_records()` returns correct count and content |
| `DeadLetterQueueTryPopAt` | `try_pop_at()` removes correct element and preserves order |
| `MailboxConfigNoDeadField` | `MailboxConfig` no longer has `enable_dead_letters` |

### 7.2. Integration tests

| Test | What it verifies |
|------|-----------------|
| `DLQReplaySuccess` | Replay delivers payload to target actor |
| `DLQReplayActorNotFound` | Replay fails cleanly when target is gone |
| `DLQReplayNoPayload` | Replay fails cleanly when record has no payload |
| `DLQOverflowHandoffEndToEnd` | Full mailbox + DeadLetter policy → DLQ record with payload |

### 7.3. CLI tests

| Test | What it verifies |
|------|-----------------|
| `CLIDlqList` | `/dlq list` produces expected table output |
| `CLIDlqListFiltered` | `/dlq list --reason X` filters correctly |
| `CLIDlqShow` | `/dlq show <n>` displays full record |
| `CLIDlqReplay` | `/dlq replay <n>` succeeds and removes record |
| `CLIDlqExportJson` | `/dlq export --format json` produces valid JSON |

## 8. Files Changed

| File | Change |
|------|--------|
| `include/hpactor/mailbox/dead_letter_queue.hpp` | Add `config()`, `snapshot_records()`, `try_pop_at()` |
| `src/mailbox/dead_letter_queue.cpp` | Implement `snapshot_records()`, `try_pop_at()` |
| `include/hpactor/mailbox/detail/overflow_context.hpp` | Add `dlq` pointer field |
| `include/hpactor/mailbox/mailbox_policy.hpp` | Remove `enable_dead_letters` field |
| `include/hpactor/core/actor_system.hpp` | Add `dead_letter_queue()` accessor |
| `src/actor/actor_system.cpp` | Fix `emit_rejection_observability()` (payload, trace, timestamp, msg param); add config gate; fix `reject_missing_actor`/`try_reject_expired` timestamps |
| `src/cli/commands/dlq_commands.hpp` | New file — CLI DLQ command handlers |
| `src/cli/commands/dlq_commands.cpp` | New file — CLI DLQ command implementations |
| `src/cli/cli_actor.cpp` | Register DLQ commands in command tree |
| `tests/unit/mailbox/test_dead_letter_queue.cpp` | Add snapshot/try_pop_at/record tests |
| `tests/integration/mailbox/test_dead_letter_queue.cpp` | Add handoff + replay integration tests |
| `tests/unit/mailbox/test_mailbox_policy.cpp` | Remove `enable_dead_letters` reference |
| `tests/system/test_cli_integration.cpp` | Add DLQ CLI command tests |

## 9. Acceptance Criteria

- [ ] When a bounded mailbox with `OverflowPolicy::DeadLetter` rejects a message,
      the resulting `DeadLetterRecord` contains the message payload (truncated to
      `max_payload_sample_bytes`), trace context, and a valid timestamp.
- [ ] `DeadLetterConfig::enabled = false` prevents DLQ record creation at the call
      site, not just inside `try_push()`.
- [ ] CLI `/dlq list`, `/dlq show <n>`, `/dlq replay <n>`, and `/dlq export`
      commands function correctly.
- [ ] `/dlq replay <n>` delivers the stored payload to the original target actor
      and removes the record on success.
- [ ] `MailboxConfig::enable_dead_letters` is removed (dead field).
- [ ] All three DLQ paths (`emit_rejection_observability`, `reject_missing_actor`,
      `try_reject_expired`) are consistent in what they record (payload, trace,
      timestamp).
- [ ] New tests pass under TSAN and ASAN.
- [ ] Existing DLQ and mailbox tests continue to pass.
