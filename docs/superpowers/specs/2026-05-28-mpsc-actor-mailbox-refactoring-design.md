# MPSCActorMailbox Refactoring — Strategy Pattern Extraction

**Date:** 2026-05-28
**Status:** Draft
**Author:** HPActor Team

## Context

`MPSCActorMailbox` in `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` has grown to 784
lines — the largest class in the codebase. It mixes nine distinct responsibilities in a
single template class:

1. Low-level MPSC queue operations (enqueue, dequeue, consumer spinlock)
2. Two-phase capacity reservation (count + bytes CAS)
3. System message protected reserve
4. Overflow policy dispatch (8-case switch with per-case metrics/counter duplication)
5. Pressure state machine (watermarks, hysteresis, state transitions)
6. Backpressure signal rate limiting (interval + escalation CAS)
7. Overflow queue draining on dequeue
8. Deferred-free lifecycle for dropped messages
9. Snapshot/inspection for CLI

The overflow policy switch alone spans ~120 lines with 6 near-identical blocks of
metrics-emission code. Phase 2 added 3 more policies; Phase 3 will add 2 more
(DropLowestPriority, BlockWhenAllowed). Adding each policy requires duplicating the
same counter+metrics+result pattern and risks inconsistent error handling.

## Goal

Extract each separable concern into its own focused class behind clear interfaces,
shrinking `MPSCActorMailbox` to a coordinator that delegates to composed components.

**Constraints:**
- Public API surface must remain identical — zero caller changes.
- No behavioral changes to capacity admission, overflow handling, pressure tracking, or
  signal gating.
- Existing tests must pass without modification beyond include adjustments.
- No RTTI, no exceptions — virtual dispatch is limited to the overflow handler interface
  (cold path only).
- Follow existing codebase conventions: header-only templates, `hpactor::mailbox`
  namespace, no heap allocation on the hot path.

## Architecture

```
mailbox/
├── mpsc_actor_mailbox.hpp              (~200 lines, down from 784)
│
└── detail/
    ├── overflow_handler_interface.hpp   (~35 lines, IOverflowHandler)
    ├── overflow_context.hpp             (~50 lines, shared context struct)
    ├── reservation_manager.hpp          (~90 lines, count+byte CAS)
    ├── pressure_state_machine.hpp       (~70 lines, watermarks + hysteresis)
    ├── backpressure_signal_gate.hpp     (~65 lines, rate limiting + escalation)
    ├── overflow_handler_factory.hpp     (~40 lines, enum → handler mapping)
    │
    └── handlers/
        ├── reject_newest_handler.hpp    (~30 lines)
        ├── drop_newest_handler.hpp      (~30 lines)
        ├── drop_oldest_handler.hpp      (~45 lines)
        ├── dead_letter_handler.hpp      (~30 lines)
        ├── signal_only_handler.hpp      (~35 lines)
        └── spill_to_overflow_handler.hpp (~40 lines)
```

### Component Responsibilities

**MPSCActorMailbox** (coordinator):
- Owns all composed components as value members
- Owns the core `MPSCMailbox<T>`, `OverflowQueue<T>`, consumer spinlock, deferred-free
  pointer, `mailbox_was_empty_` flag, simple counters (`total_enqueued_`, `total_dequeued_`,
  `max_depth_`)
- `try_push()`: reserve → on failure, delegate to `IOverflowHandler::handle()` → enqueue
- `dequeue()`: dequeue from MPSC queue → release reservation → drain overflow → return
- `snapshot()`: reads from reservation manager and overflow queue
- Public methods: unchanged signatures, delegate internally

**IOverflowHandler** (strategy interface):
```cpp
class IOverflowHandler {
  public:
    virtual ~IOverflowHandler() = default;
    virtual EnqueueResult handle(OverflowContext& ctx,
                                 ReservationResult reason) = 0;
    virtual OverflowPolicy policy() const = 0;
};
```
- One virtual call per rejection — the overflow path is cold (only hit at capacity).
- Each concrete handler is stateless; all state lives in `OverflowContext` or is owned
  by the mailbox.

**OverflowContext**:
```cpp
struct OverflowContext {
    const T& message;
    MailboxEnvelopeMeta& meta;
    ReservationManager& reservation;
    OverflowQueue<T>& overflow_queue;
    std::atomic<uint64_t>& total_rejected;
    std::atomic<uint64_t>& total_dropped;
    std::atomic<uint64_t>& total_dead_letters;
    metrics::MpscRingBuffer<MetricEvent>* metrics_buf;
    MailboxConfig& config;
    ActorId actor_id;
    uint32_t current_depth;            // filled by mailbox before handler call
    uint64_t current_bytes;            // filled by mailbox before handler call
    std::function<bool()> drop_oldest_fn;  // bound to drop_one_oldest(), nullable
};
```
- Stack-allocated by `try_push()` on each rejection, passed by reference.
- `current_depth` and `current_bytes` let each handler construct a complete
  `EnqueueResult` with correct depth/bytes/capacity/ratio fields.
- `drop_oldest_fn` is bound by the mailbox to `drop_one_oldest()` — only
  `DropOldestHandler` calls it. Returns false if no message could be dropped.
- Gives each handler exactly what it needs — no handler touches data it doesn't use.

**ReservationManager**:
- Owns `reserved_messages_`, `reserved_system_messages_`, `queued_bytes_` atomics.
- `try_reserve(bytes, max_messages, max_bytes) → ReservationResult` — two-phase CAS
  (count then bytes, rollback count on byte failure).
- `try_reserve_system(bytes, limit) → bool` — CAS on system reserve.
- `release(bytes)`, `release_system(bytes)` — release from appropriate pool.
- `reserved_count()`, `queued_bytes()` — for snapshot.

**PressureStateMachine**:
- Owns `pressure_state_` atomic.
- `update(ratio, hard_failure, watermarks) → void` — drives state transitions.
- `current_state() → MailboxPressureState`
- `code_after_accept() → EnqueueResultCode` — Accepted vs AcceptedWithSoftPressure.
- `pressure_severity(state) → uint8_t` — for signal escalation.

**BackpressureSignalGate**:
- Owns `last_signal_ns_`, `last_severity_`, `sequence_` atomics.
- `try_acquire(now_ns, state, interval_ms) → optional<uint64_t>` — rate-limited CAS
  with escalation bypass.

**OverflowHandlerFactory**:
- Free function: `make_overflow_handler(OverflowPolicy) → unique_ptr<IOverflowHandler>`
- Maps each enum value to its concrete handler.
- Unimplemented policies (BlockWhenAllowed, DropLowestPriority) get `RejectNewestHandler`
  with a log warning — removes the catch-all "not yet implemented" fallthrough.

### Concrete Handlers

| Handler | Policy | Behavior |
|---------|--------|----------|
| `RejectNewestHandler` | `RejectNewest` | Increment rejected, emit metric, return Rejected |
| `DropNewestHandler` | `DropNewest` | Increment dropped, emit metric, return DroppedNewest |
| `DropOldestHandler` | `DropOldest` | Call ctx.drop_oldest_fn(); if true, return DroppedExisting and mailbox retries reservation; if false, return Rejected |
| `DeadLetterHandler` | `DeadLetter` | Increment dead_letters, emit metric, return ReroutedToDeadLetter |
| `SignalOnlyHandler` | `SignalOnly` | Increment rejected, emit metric, return Rejected with retry_after |
| `SpillToOverflowHandler` | `SpillToOverflow` | Try overflow_queue.push, emit metric, return ReroutedToOverflow or Rejected |

### Inter-component Data Flow (try_push)

```
caller
  └─ MPSCActorMailbox::try_push(msg, meta)
       ├─ reservation_.try_reserve(bytes)
       │    └─ [Reserved] → allocate + enqueue_reserved + pressure update → return Accepted
       │    └─ [Rejected]
       │         ├─ is_system? → reservation_.try_reserve_system(bytes)
       │         │    └─ [true] → allocate + enqueue (bypasses overflow)
       │         └─ [false]
       │              ├─ pressure_state_.update(hard_failure=true)
       │              ├─ build OverflowContext on stack (fill depth/bytes/drop_oldest_fn)
       │              ├─ result = overflow_handler_->handle(ctx, reserve_reason)
       │              ├─ [DropOldest success] → retry try_reserve()
       │              │    └─ [Reserved] → allocate + enqueue → return Accepted
       │              │    └─ [Rejected] → return result (Rejected)
       │              └─ [all other policies] → return result
       └─ [enqueue path]
            ├─ mem::allocate + placement new
            ├─ enqueue_reserved → mailbox_.enqueue → notify scheduler
            └─ return make_result(pressure_state_.code_after_accept())
```

### Inter-component Data Flow (dequeue with drain)

```
caller
  └─ MPSCActorMailbox::dequeue()
       ├─ lock_consumer()
       ├─ mailbox_.dequeue()
       ├─ reservation_.release(bytes)   [or release_system for system msgs]
       ├─ drain_overflow()
       │    └─ while overflow_queue not empty:
       │         ├─ reservation_.try_reserve(0)   [count-only]
       │         ├─ overflow_queue_.try_pop()
       │         └─ enqueue_reserved(suppress_wakeup=true)
       ├─ unlock_consumer()
       └─ return node
```

### Config Propagation

`set_config()` now does three things:
1. Store the new config value.
2. `overflow_handler_ = make_overflow_handler(config_.overflow_policy)` — swap strategy.
3. `overflow_queue_.set_max_depth(config_.max_overflow_depth)` — reconfig overflow.

The handler is recreated on config change. Since config changes are not concurrent
with `try_push()` (per the existing doc comment), this is safe.

## What Stays Unchanged

- `MPSCMailbox<T>` — the lock-free Vyukov queue (no changes).
- `OverflowQueue<T>` — the mutex-guarded spill queue (no changes).
- `MailboxPolicy` types — all enums and config structs in `mailbox_policy.hpp` (no changes).
- Consumer spinlock — `lock_consumer()`/`unlock_consumer()` remain private in the mailbox.
- `drop_one_oldest()` — stays in the mailbox (needs access to MPSCMailbox internals).
- `drain_overflow()` — stays in the mailbox (needs spinlock + enqueue_reserved).
- Deferred-free (`pending_free_`) — stays in the mailbox (coupled to consumer lifecycle).
- `enqueue_reserved()`, `inject_for_test()`, `estimate_node_bytes()` — stay as private helpers.

## Template Consideration

`MPSCActorMailbox` remains templated on `<typename T>`. All detail components in
`mailbox/detail/` are also templated on `T` where needed. In practice `T = TypedMessage`
everywhere, but the template parameter is preserved for consistency with the existing API
and potential future use.

`IOverflowHandler<T>` is a template — it references `OverflowContext<T>` which carries
the message as `const T&`. Each concrete handler is templated on `T` and the factory
function `make_overflow_handler<T>(OverflowPolicy) → unique_ptr<IOverflowHandler<T>>`
is also templated. In practice `T = TypedMessage` everywhere.

## Testing Strategy

Each new component gets a focused unit test:

| Test file | What it covers |
|-----------|---------------|
| `test_reservation_manager` | Two-phase CAS, rollback, system reserve, release accounting |
| `test_pressure_state_machine` | Watermark thresholds, hysteresis, HardPressure→Recovering→Normal |
| `test_backpressure_signal_gate` | Interval gating, escalation bypass, sequence numbering |
| `test_overflow_handlers` | Each handler returns correct EnqueueResult, emits correct metric |
| `test_overflow_handler_factory` | Enum-to-handler mapping, unknown policy fallback |

Existing tests in `test_bounded_mailbox`, `test_mailbox_overflow_policies`,
`test_mpsc_actor_mailbox`, `test_mailbox_awaiter`, `test_mailbox_backpressure_stress`,
and `test_scheduler_control` continue to pass without modification since the public API
is unchanged.

## Files Changed

**New files (12):**
- `include/hpactor/mailbox/detail/overflow_handler_interface.hpp`
- `include/hpactor/mailbox/detail/overflow_context.hpp`
- `include/hpactor/mailbox/detail/reservation_manager.hpp`
- `include/hpactor/mailbox/detail/pressure_state_machine.hpp`
- `include/hpactor/mailbox/detail/backpressure_signal_gate.hpp`
- `include/hpactor/mailbox/detail/overflow_handler_factory.hpp`
- `include/hpactor/mailbox/detail/handlers/reject_newest_handler.hpp`
- `include/hpactor/mailbox/detail/handlers/drop_newest_handler.hpp`
- `include/hpactor/mailbox/detail/handlers/drop_oldest_handler.hpp`
- `include/hpactor/mailbox/detail/handlers/dead_letter_handler.hpp`
- `include/hpactor/mailbox/detail/handlers/signal_only_handler.hpp`
- `include/hpactor/mailbox/detail/handlers/spill_to_overflow_handler.hpp`

**Modified files (1):**
- `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` — replace internals, keep public API.

**New test files (5):**
- `tests/unit/mailbox/test_reservation_manager.cpp`
- `tests/unit/mailbox/test_pressure_state_machine.cpp`
- `tests/unit/mailbox/test_backpressure_signal_gate.cpp`
- `tests/unit/mailbox/test_overflow_handlers.cpp`
- `tests/unit/mailbox/test_overflow_handler_factory.cpp`

**No changes to:**
- `mailbox_policy.hpp` — all public types unchanged.
- `mpsc_mailbox.hpp` — lock-free queue unchanged.
- `overflow_queue.hpp` — unchanged.
- Any consumer of `MPSCActorMailbox` — public API identical.
- `CMakeLists.txt` test registration (if `tests/unit/mailbox/CMakeLists.txt` uses
  globbing; otherwise add the 5 new test sources).
