# Actor Link and Monitor Design Spec

## Overview

This document specifies the implementation of actor linking (bidirectional death sharing) and monitoring (one-way death watching) for HPActor. The `TypeTag` enum, protobuf message types, and stub API surface already exist — this spec covers the runtime logic that connects them.

---

## Concepts (from actor-core-concept.md)

| | Linking | Monitoring |
|---|---|---|
| Scope | Bidirectional | One-way |
| Use case | Death sharing | Watching |
| Effect | Either death affects the other | Only target death is observed |

**Linking:** If actor A links to B, and B dies, A receives `DownMsg`. If A dies, B also receives `DownMsg`. Either actor dying affects the other.

**Monitoring:** If A monitors B, and B dies, A receives `DownMsg`. B is unaffected by A's death.

---

## Current State

### Already in place

| Artifact | Location | Status |
|----------|----------|--------|
| `TypeTag::DownMsg` (1) | `types/types.hpp:437` | Defined |
| `TypeTag::ExitMsg` (2) | `types/types.hpp:438` | Defined |
| `TypeTag::LinkMsg` (3) | `types/types.hpp:439` | Defined |
| `TypeTag::UnlinkMsg` (4) | `types/types.hpp:440` | Defined |
| `errors::actor_down` (2) | `types/types.hpp:308` | Defined |
| `DownMessage` proto | `protos/hpactor/messages.proto:11` | Defined |
| `ExitMessage` proto | `protos/hpactor/messages.proto:17` | Defined |
| `LinkMessage` proto | `protos/hpactor/messages.proto:23` | Defined |
| `UnlinkMessage` proto | `protos/hpactor/messages.proto:27` | Defined |
| Proto registry entries | `src/core/proto_type_registry.cpp:23-26` | Registered |
| `AbstractActor::link_to()` | `abstract_actor.hpp:73` | Stub |
| `AbstractActor::unlink_from()` | `abstract_actor.hpp:74` | Stub |
| `AbstractActor::monitor()` | `abstract_actor.hpp:77` | Stub |
| `AbstractActor::demonitor()` | `abstract_actor.hpp:78` | Stub |
| `AbstractActor::on_exit()` | `event_based_actor.hpp:239` | Virtual, called by scheduler on termination |
| `LocalActor::on_activate()` | `local_actor.hpp:43` | Virtual, called on spawn |
| `ActorContext::linked_` | `actor_context.hpp:117` | Vector, populated by... nothing currently |
| `ActorContext::monitored_` | `actor_context.hpp:118` | Vector |
| `ActorContext::monitor()` | `actor_context.hpp:102` | **Only appends to `monitored_`** — does not interact with target |
| `ActorContext::linked_actors()` | `actor_context.hpp:99` | Getter only |
| `SupervisorActor` handles `DownMsg` | `src/supervision/supervision.cpp:51` | Supervision restart logic |
| `ActorSystem::deliver_local()` | `actor_system.hpp:167` | Local message delivery works |

### Missing pieces

1. **`link_to()`** — does not send `LinkMsg` to target or populate `linked_`
2. **`unlink_from()`** — does not send `UnlinkMsg` or clean up
3. **`monitor()` (AbstractActor)** — stub, no logic
4. **`demonitor()`** — stub, no logic
5. **System message dispatch** — `EventBasedActor::receive()` does not dispatch `LinkMsg`/`UnlinkMsg`/`DownMsg`/`ExitMsg` to user handlers. Currently `SupervisorActor` handles `DownMsg` manually in its own `make_behavior()`.
6. **Death propagation** — when an actor terminates, no `DownMsg` is sent to linked/monitored actors
7. **Remote linking/monitoring** — protocol exists (protobuf types) but runtime is not wired

---

## Design

### 1. Link Protocol (Bidirectional)

```
A.link_to(B_addr)
    │
    ├─ Add B_addr to A.context()->linked_
    ├─ Serialize LinkMessage{ target=B_addr, actor_id=A.id }
    ├─ Send TypedMessage(TypeTag::LinkMsg, serialized) to B
    │
    ▼
B receives LinkMsg
    │
    └─ Add A_addr (from msg.sender_address()) to B.context()->linked_
```

**Invariant:** After `A.link_to(B)` completes, both A and B have each other in `linked_`. If either dies, the other receives `DownMsg`.

**`link_to()` implementation on `AbstractActor`:**
```cpp
void AbstractActor::link_to(const ActorAddr& other) {
    // 1. Add to local linked list
    context()->linked_.push_back(other);

    // 2. Notify target via LinkMessage
    LinkMessage pb;
    *pb.mutable_target() = encode_endpoint(other);
    pb.set_actor_id(id().value());

    bytes payload(pb.ByteSizeLong());
    pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    auto msg = TypedMessage(TypeTag::LinkMsg, std::move(payload));
    context()->send(other, std::move(msg));
}
```

**`unlink_from()`:**
```cpp
void AbstractActor::unlink_from(const ActorAddr& other) {
    // 1. Remove from local linked list
    auto& linked = context()->linked_;
    linked.erase(std::remove_if(linked.begin(), linked.end(),
        [&](const ActorAddress& a) { return a == other; }), linked.end());

    // 2. Notify target via UnlinkMessage
    UnlinkMessage pb;
    *pb.mutable_target() = encode_endpoint(other);
    pb.set_actor_id(id().value());

    bytes payload(pb.ByteSizeLong());
    pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    auto msg = TypedMessage(TypeTag::UnlinkMsg, std::move(payload));
    context()->send(other, std::move(msg));
}
```

### 2. Monitor Protocol (One-Way)

```
A.monitor(B_addr)
    │
    └─ Add B_addr to A.context()->monitored_
       (No message sent to B — one-way)
```

```cpp
void AbstractActor::monitor(const ActorAddr& target) {
    context()->monitored_.push_back(target);
}

void AbstractActor::demonitor(const ActorAddr& target) {
    auto& monitored = context()->monitored_;
    monitored.erase(std::remove_if(monitored.begin(), monitored.end(),
        [&](const ActorAddress& a) { return a == target; }), monitored.end());
}
```

Note: `ActorContext` already has a `monitor()` method — it should be unified. The existing `ActorContext::monitor()` only appends to `monitored_`. The `AbstractActor` methods should operate through the context.

### 3. System Message Dispatch in EventBasedActor::receive()

`EventBasedActor::receive()` must intercept system messages before delegating to the user behavior:

```
receive(TypedMessage& msg)
    │
    ├─ type == LinkMsg   → handle_link_msg(msg)   // Add sender to linked_
    ├─ type == UnlinkMsg → handle_unlink_msg(msg)  // Remove sender from linked_
    ├─ type == DownMsg   → handle_down_msg(msg)    // Forward to behavior + notify children
    ├─ type == ExitMsg   → handle_exit_msg(msg)    // Forward to behavior
    └─ otherwise         → behavior_(msg)           // User handler
```

**`handle_link_msg()`:**
- Parse `LinkMessage` from payload
- Add `msg.sender_address()` to `linked_` (the remote end of the bidirectional link)
- Do NOT forward to user behavior (system-internal)

**`handle_unlink_msg()`:**
- Parse `UnlinkMessage` from payload
- Remove `msg.sender_address()` from `linked_`
- Do NOT forward to user behavior

**`handle_down_msg()`:**
- Parse `DownMessage` from payload
- Remove the dead actor from `linked_` and `monitored_` (cleanup)
- Forward to user behavior so supervision/application logic can react
- This preserves the existing `SupervisorActor` pattern which already catches `DownMsg` in `make_behavior()`

**`handle_exit_msg()`:**
- Forward to user behavior for graceful shutdown handling

### 4. Death Propagation

When an actor terminates, `on_exit()` is called. Override in `EventBasedActor` to propagate death:

```
EventBasedActor::on_exit()
    │
    ├─ DownMessage pb{ actor_id=this.id(), reason_code=exit_reason_ }
    ├─ For each addr in linked_:
    │     send TypedMessage(TypeTag::DownMsg, pb) to addr
    ├─ For each addr in monitored_:
    │     send TypedMessage(TypeTag::DownMsg, pb) to addr
    └─ Clear linked_, monitored_
```

The `exit_reason_` member tracks why the actor is stopping (normal exit = 0, failure = error code). This requires adding a `uint32_t exit_reason_` field to `EventBasedActor`, set by a new `set_exit_reason(uint32_t)` method or by the scheduler when an exception/unhandled error occurs.

**Alternative (simpler initial implementation):** Call `send_down_notifications()` directly from `on_exit()` with reason code 0 (normal). The scheduler sets a non-zero reason for crash/error termination paths.

### 5. ActorContext Changes

`ActorContext` needs `linked_` to be mutable from `AbstractActor`. The existing `linked_` vector is private. Options:

- **Option A:** Add `add_linked()` / `remove_linked()` methods to `ActorContext` (clean encapsulation)
- **Option B:** Make `AbstractActor` a friend of `ActorContext` (simpler, less boilerplate)

Recommend **Option A** — add two methods:

```cpp
// In ActorContext:
void add_linked(const ActorAddress& addr) { linked_.push_back(addr); }
void remove_linked(const ActorAddress& addr) {
    linked_.erase(std::remove(linked_.begin(), linked_.end(), addr), linked_.end());
}
```

Similarly for `monitored_`:
```cpp
void add_monitored(const ActorAddress& addr) { monitored_.push_back(addr); }
void remove_monitored(const ActorAddress& addr) {
    monitored_.erase(std::remove(monitored_.begin(), monitored_.end(), addr),
                     monitored_.end());
}
```

### 6. Remote Linking/Monitoring

For non-local targets, `LinkMsg`/`UnlinkMsg`/`DownMsg` use protobuf serialization (already defined). The existing `ActorContext::send()` resolves the target address and routes through `ActorProxy` for remote endpoints — this path already works.

No additional wire protocol changes are needed. The protobuf types (`LinkMessage`, `UnlinkMessage`, `DownMessage`) carry `PbActorEndpoint` which includes node routing info.

---

## Implementation Plan

### Phase 1: Local linking/monitoring (core)

| Step | File | Change |
|------|------|--------|
| 1 | `actor_context.hpp` | Add `add_linked()`, `remove_linked()`, `add_monitored()`, `remove_monitored()` methods |
| 2 | `abstract_actor.cpp` | Implement `link_to()`, `unlink_from()`, `monitor()`, `demonitor()` using context methods |
| 3 | `event_based_actor.hpp` | Add `exit_reason_` field; add `set_exit_reason()` |
| 4 | `event_based_actor.hpp` / `.cpp` | Override `on_exit()` to send `DownMsg` to linked + monitored actors |
| 5 | `event_based_actor.hpp` / `.cpp` | Override `receive()` to intercept `LinkMsg`, `UnlinkMsg` before delegating to behavior |

### Phase 2: Scheduler integration

| Step | File | Change |
|------|------|--------|
| 6 | `scheduler.cpp` | Call `actor->set_exit_reason(code)` before `actor->on_exit()` in termination paths |
| 7 | `scheduler.cpp` | Ensure coroutine path also triggers death propagation (coroutine `co_return` → `on_exit()`) |

### Phase 3: Supervision wiring

| Step | File | Change |
|------|------|--------|
| 8 | `supervision.cpp` | Update `SupervisorActor` to use the new `receive()` dispatch (no behavior change needed — `DownMsg` still reaches `make_behavior()`) |
| 9 | `event_based_actor.hpp` | Remove `become_empty()` now-unnecessary direct `DownMsg` interception in subclasses |

### Phase 4: Testing

| Step | File | Change |
|------|------|--------|
| 10 | `tests/` | Test: `link_to` bidirectional — both actors receive `DownMsg` on death |
| 11 | `tests/` | Test: `monitor` one-way — only monitor receives `DownMsg` |
| 12 | `tests/` | Test: `unlink_from` — link removed, no `DownMsg` sent after unlink |
| 13 | `tests/` | Test: `demonitor` — monitoring stopped |
| 14 | `tests/` | Test: multiple links — all linked actors notified |
| 15 | `tests/` | Test: link to already-dead actor — graceful handling |

---

## Interaction with Supervision

Supervision and linking are orthogonal but complementary:

- **Supervision** is hierarchical (parent → child) with restart policy
- **Linking** is peer-to-peer (actor ↔ actor) with death notification only
- **Monitoring** is one-way (watcher → target) with death notification only

When a parent spawns a child, the framework should **automatically monitor** the child (not link — the parent doesn't want to die if the child dies). The parent's supervision logic reacts to `DownMsg` from monitored children, applying restart/stop/escalate directives.

The existing `SupervisorActor::make_behavior()` already handles `DownMsg` — after this implementation, the message dispatch is automatic (no behavior change required in supervisor code).

---

## Edge Cases

| Case | Behavior |
|------|----------|
| Link to self | Reject — no-op, log warning |
| Duplicate link | Idempotent — check before adding to `linked_` |
| Link to dead actor | `DownMsg` delivered immediately to linker (or on next scheduler tick) |
| Actor dies during link setup | `LinkMsg` may arrive at dead actor → silently drop |
| Remote actor dies | `DownMsg` arrives via network; local `linked_`/`monitored_` cleaned up |
| Cyclic links (A↔B↔C↔A) | No deadlock risk — `DownMsg` is fire-and-forget. Each actor independently cleans up |
| Actor with no context (system actor) | `context()` is null → link/monitor is no-op |

---

## Open Questions

1. **Should `spawn()` auto-link/monitor the child?** CAF automatically monitors children. Recommend: `spawn()` adds an implicit monitor (parent watches child). This aligns with the existing `ActorContext::children_` list and supervision tree concept.

2. **Should `DownMsg` include incarnation?** The `ActorAddress` has an `incarnation` field that increments on restart. Including incarnation in `DownMessage` lets receivers distinguish "actor died" from "actor restarted." Recommend: add `incarnation` field to `DownMessage` proto.

3. **Exit reason granularity?** Currently `DownMessage` has a `uint32 reason_code`. For the initial implementation, use `errors::actor_down` (2) for abnormal termination and `0` for normal exit. Extend later with more specific codes.

4. **Coroutine path death propagation?** When a coroutine-based actor `co_return`s, `execute_actor()` calls `coroutine.done()` then `actor->on_exit()`. The coroutine path must also call `on_exit()` — verified that it does (line 238 in `scheduler.cpp`).
