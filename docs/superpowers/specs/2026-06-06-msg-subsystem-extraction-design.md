# Msg Subsystem Extraction Design

**Date:** 2026-06-06
**Status:** Design approved, awaiting implementation plan

## 1. Motivation

Message-related types in HPActor are currently spread across four subsystems:

| Current Location | Types |
|---|---|
| `types/` | `TypedMessage`, `TypeTag`, `MessageId`, `FailureReason`, `FailureEnvelope`, `RequestHandle<T>`, `RequestTimeout` |
| `actor/` | `TypedMessage` class definition (separate from `types/types.hpp`) |
| `mailbox/` | `DeliveryMode`, `DeliveryResult`, `EnqueueResultCode`, `EnqueueResult`, `DeadLetterRecord`, `DedupCache` |
| `net/` | `WireFrame`, frame encoding/decoding |

The high-level architecture doc defines a `Message` struct as the sole communication primitive. The low-level design doc defines `TypedMessage` as the "single message type flowing through all paths." Yet there is no single subsystem that owns the message lifecycle vocabulary — delivery contracts are mailbox-owned, wire encoding is network-owned, failure envelopes are in `types/`, and the message class itself is split between `actor/` and `types/`.

Extracting these into a cohesive `msg/` subsystem gives the message lifecycle a clear architectural home.

## 2. Goals

1. Consolidate message identity, delivery contract, failure, framing, and request/response types into a single `msg/` subsystem
2. Establish clear dependency rules: `msg/` is a leaf-level type subsystem consumed by all higher subsystems
3. Preserve source compatibility during migration via compatibility shims
4. No runtime behavior change — this is a pure code organization refactor

## 3. Non-Goals

- Moving runtime components (`AskManager`, `DeadLetterQueue` actor, `RpcChannel`, `MPSCActorMailbox`) — these stay in their current subsystems
- Moving `TraceContext` — stays in `types/` as a shared primitive used by both `msg/` and `tracing/`
- Moving `ActorAddress` — stays in `ref/` as a reference/routing concept
- Changing any public API signatures or behavior
- Creating new `.cpp` translation units — `msg/` is header-only

## 4. Design

### 4.1 Subsystem Contents

```
include/hpactor/msg/
├── typed_message.hpp       # TypedMessage class (from actor/typed_message.hpp)
├── type_tag.hpp            # TypeTag enum (from types/types.hpp)
├── message_id.hpp          # MessageId alias, MessageTag (from types/types.hpp + adt/tags.hpp)
├── frame.hpp               # WireFrame, FrameFlag, encode/decode (from net/frame.hpp)
├── delivery_mode.hpp       # DeliveryMode enum (from mailbox/delivery_mode.hpp)
├── delivery_result.hpp     # DeliveryResult struct (from mailbox/delivery_result.hpp)
├── enqueue_result.hpp      # EnqueueResultCode enum + EnqueueResult struct (from mailbox/mailbox_policy.hpp)
├── dedup_cache.hpp         # DedupCache (from mailbox/dedup_cache.hpp)
├── dead_letter_record.hpp  # DeadLetterRecord struct (from mailbox/dead_letter_queue.hpp)
├── failure_reason.hpp      # FailureReason + FailureSource enums (from types/failure_reason.hpp)
├── failure_envelope.hpp    # FailureEnvelope struct (from types/failure_envelope.hpp)
├── request_handle.hpp      # RequestHandle<T> (from types/request_handle.hpp)
├── request_timeout.hpp     # RequestTimeout (from types/request_timeout.hpp)
└── fwd.hpp                 # Forward declarations for the subsystem
```

### 4.2 What Stays Behind

| Stays in | Type | Reason |
|----------|------|--------|
| `types/` | `TraceContext` | Shared primitive used by both `msg/` and `tracing/` |
| `types/` | `error`, `result<T>` | General-purpose, not message-specific |
| `types/` | `AlarmHandle`, `Clock` | Timer/scheduling, not message |
| `ref/` | `ActorAddress` | Reference/routing concept |
| `adt/` | `Id<Tag,T>`, `ActorTag`, `TimerTag` | Generic ADT; only `MessageTag` moves to `msg/` |
| `mailbox/` | `DeadLetterQueue` actor | Runtime infrastructure, consumes `msg/` types |
| `mailbox/` | `MPSCActorMailbox`, `MultiLaneQueue` | Mailbox data structures |
| `mailbox/` | Overflow handlers, pressure state machine | Mailbox internals |
| `actor/` | `AskManager` | Runtime component, consumes `msg/` types |
| `rpc/` | `RpcChannel`, `RpcFuture` | Runtime components, consume `msg/` types |
| `net/` | `WireFrameConnection` | Transport connection, consumes `WireFrame` |
| `net/` | `PbActorAddress` ↔ `ActorAddress` conversion utilities | Address conversion, not message framing |

### 4.3 Dependency Graph

**What `msg/` depends ON (leaf-level, minimal):**

```
msg/
  ├── adt/id.hpp              → Id<Tag,T> template
  ├── adt/stream_buffer.hpp   → StreamBuffer (payload storage)
  ├── adt/tags.hpp            → MessageTag
  ├── ref/actor_address.hpp   → ActorAddress (sender/receiver fields)
  ├── types/types_fwd.hpp     → TraceContext forward declaration
  └── <protobuf>              → PbActorAddress, PbActorMsgFrame (WireFrame encode/decode)
```

`msg/` never includes headers from `actor/`, `mailbox/`, `core/`, `net/`, `sched/`, `rpc/`, `config/`, `cli/`, `supervision/`, `tracing/`, `metrics/`, or `log/`.

**What depends ON `msg/` (broad — foundational type subsystem):**

```
actor/       ─┐
mailbox/      │
net/          │
core/         ├── depends on msg/ (TypedMessage, TypeTag, DeliveryResult,
rpc/          │    EnqueueResult, FailureEnvelope, WireFrame, etc.)
config/       │
spawn/        │
supervision/  │
tracing/      │
cli/          │
metrics/      │
log/         ─┘
```

### 4.4 Dependency Rules

- `msg/` is purely data types — no I/O, no threads, no locks, no actor calls
- `msg/` does not know about actors, schedulers, transports, or mailboxes
- `msg/` types may have methods for conversion, validation, and serialization, but not for I/O or scheduling

## 5. Migration Strategy

Three phases with compatibility shims to avoid a single massive change.

### Phase 1: Create `msg/`, Move Definitions, Add Shims

1. Create `include/hpactor/msg/` with all 14 headers containing the canonical definitions
2. Move type definitions: copy content from old locations to new `msg/` headers, updating internal `#include` paths
3. Replace old headers with thin compatibility shims:

```cpp
// include/hpactor/types/failure_reason.hpp  (SHIM)
#pragma once
#include <hpactor/msg/failure_reason.hpp>
// Compatibility shim: FailureReason moved to msg/ subsystem.
// TODO: update consumers to include <hpactor/msg/failure_reason.hpp> directly.
```

4. Add `include/hpactor/msg/fwd.hpp` with forward declarations for all `msg/` types
5. Build and full test run to verify shims work transparently

### Phase 2: Update Internal Consumers

1. Update every `#include` in `src/`, `tests/`, `examples/`, `apps/`, `tools/` to use `msg/` paths directly instead of shim paths
2. Update forward declarations of moved types in non-`msg/` headers (e.g., `behavior.hpp` forward-declaring `TypedMessage` → include `msg/fwd.hpp`)
3. Build and full test run after each subsystem is updated

### Phase 3: Remove Shims (Optional, Can Defer)

1. Delete old compatibility shim headers once all consumers are updated
2. Clean up any remaining references to old paths
3. Final build and full test run

### Partial Extractions

Some source files provide only part of their content to `msg/`:

| Source File | What Moves to `msg/` | What Stays |
|---|---|---|
| `types/types.hpp` | `TypeTag` enum, `MessageId` alias, `TypedMessage` class | `TraceContext`, `error`, `result<T>`, `Clock` |
| `mailbox/mailbox_policy.hpp` | `EnqueueResultCode`, `EnqueueResult` | `MailboxPolicyConfig`, `MailboxPolicy` |
| `mailbox/dead_letter_queue.hpp` | `DeadLetterRecord` struct | `DeadLetterQueue` class |

## 6. Build Integration

- **No new CMake library target** — `msg/` is header-only
- **No new `.cpp` files** — existing `.cpp` files (`src/types/failure_reason.cpp`, `src/net/frame.cpp`) update their includes; relocation to `src/msg/` is a potential follow-up
- `include/hpactor/msg/` is installed alongside existing headers
- All existing CMake targets (`hpactor_lib`, test binaries, examples, apps) continue to build without CMake changes

## 7. Compatibility

### 7.1 Preserved

- All public type names, method signatures, enum values
- All namespace qualifications (`hpactor::TypedMessage`, etc.)
- All `#include` paths remain valid via Phase 1 shims
- No ABI changes (same types, same layout)
- No behavioral changes

### 7.2 Modified

- Canonical header location for moved types changes from old path to `msg/` path
- `MessageTag` moves from `adt/tags.hpp` to `msg/message_id.hpp`
- Consumers that were only getting `TypedMessage` through transitive includes from `types/types.hpp` or `actor/typed_message.hpp` will get it through `msg/` shims or direct `msg/` includes

### 7.3 Risks

| Risk | Mitigation |
|------|-----------|
| ~200+ files need include path updates | Phase 1 shims make all old paths work; Phase 2 is incremental |
| `TypeTag` is large (~500 lines) and frequently edited | Single location `msg/type_tag.hpp` — easier to find than buried at line 531 of `types/types.hpp` |
| `mailbox/mailbox_policy.hpp` split — `EnqueueResult` moves but `MailboxPolicy` stays | `EnqueueResult` already has independent forward decl; extraction is clean |
| `DeadLetterRecord` split from `DeadLetterQueue` | `DeadLetterRecord` is a standalone struct with no dependency on `DeadLetterQueue` |
| Transitive include breakage | Phase 1 shims preserve all existing include paths |

## 8. Test Plan

1. **Build verification:** Full `ninja -C build` after each phase
2. **Test suite:** `ctest --output-on-failure --parallel 8` after each phase — all 32 GTest binaries must pass
3. **Sanitizer check:** `ENABLE_ASAN=ON` build and test run for at least one configuration
4. **Include hygiene:** Verify `msg/` headers compile in isolation (each `msg/*.hpp` includes everything it needs)

## 9. Relationship to Architecture Docs

This extraction aligns with:

- **High-level design** (`system-architecture-and-key-concept-low-level-design.md` §1.4): "Message — the sole communication primitive between actors"
- **Unified message passing** (`core/unified-message-passing.md`): "TypedMessage is the single message type flowing through all paths"
- **Delivery semantics** (`production/actor-delivery-semantics-design.md`): delivery contracts now owned by `msg/`
- **Structured failure envelope** (`production/structured-failure-envelope-design.md`): failure types now owned by `msg/`
- **Dead-letter queue** (`production/dead-letter-queue-design.md`): `DeadLetterRecord` now in `msg/`, `DeadLetterQueue` actor stays in `mailbox/`

The `msg/` subsystem becomes the single source of truth for: what a message is, how it's identified, how delivery outcomes are described, how it's framed for the wire, and how failures are recorded.
