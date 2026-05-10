# Actor Core Design Concept

## Overview

HPActor is a C++20 event-based actor framework inspired by CAF (C++ Actor Framework). The core model is **event-driven actors with turn-based concurrency** — actors process messages cooperatively, one at a time, eliminating data races on actor state.

**Core Principle:** Actors are independent units of computation that communicate exclusively through asynchronous message passing. No shared mutable state between actors.

---

## Actor Model

### Fundamental Rules

1. **No shared state** — Actors encapsulate private state; external access only via messages
2. **Mailbox ordering** — Messages arrive FIFO by default; priority-aware mailboxes preserve FIFO within each priority lane
3. **Location transparency** — Actors are addressed by address, not reference
4. **Hierarchy** — Actors form trees; parents supervise children

### Turn-Based Concurrency

An actor processes a single message to completion before accepting the next. This means:
- No locking needed for actor-internal state
- State mutations are safe between message handlers
- Deadlocks avoided (no blocking in message handlers for event-based actors)

---

## Actor Type Hierarchy

```
AbstractActor (interface base, receive() = 0)
    │
    └── LocalActor (ActorContext access)
            │
            ├── EventBasedActor (cooperative scheduling, behavior + protobuf handlers)
            │       ├── StatefulActor<T> (explicit state struct)
            │       ├── ProtoStatefulActor<T> (protobuf-native + explicit state)
            │       ├── SpawnReceiver (system actor for remote spawn)
            │       └── DenseComputingActor (dedicated pool dispatch)
            │
            ├── TypedEventBasedActor<Signatures...> (static typing)
            │
            └── BlockingActor (thread-per-actor, blocking receive)
                    └── ScopedActor (main thread / non-actor contexts)
```

### AbstractActor

Base for all actors providing:
- `id()` — unique ActorId
- `address()` — ActorAddress (node, type, id, incarnation)
- `system()` — reference to ActorSystem
- `link_to()` / `unlink_from()` — death sharing
- `monitor()` / `demonitor()` — one-way death watching
- `receive(MessageVariant&&)` — message processing (pure virtual)

### EventBasedActor

Cooperative, event-driven actors using **Behavior** objects:

```cpp
class EventBasedActor : public LocalActor {
    void become(Behavior bh);      // Switch behavior at runtime
    void become_empty();            // Drop incoming messages
    virtual Behavior make_behavior(); // Override for initial behavior
};
```

**Behavior** wraps a `std::function<void(MessageVariant&&)>` and enables dynamic behavior switching — the foundation for state machines and protocol handlers.

### StatefulActor<T>

An EventBasedActor with an explicit state object of type `T`:

```cpp
template<typename T> class StatefulActor : public EventBasedActor {
    T& state();       // Mutable access
    const T& state() const;  // Immutable access
};
```

State persists across message handlers. `T` can be any struct/class.

### TypedEventBasedActor<Signatures...>

Statically typed actors where signatures are declared at compile time:

```cpp
template<typename... Signatures>
class TypedEventBasedActor : public LocalActor {
    using behavior_type = TypedBehavior<Signatures...>;
    void become(behavior_type bh);
};
```

Signature format: `result<ReturnType>(MessageType)`

### BlockingActor

Actors that run in their own thread with blocking receive. Use when you need to call blocking APIs or use synchronous wait patterns.

### ScopedActor

A BlockingActor for non-actor contexts (e.g., `main()`). Allows the main thread to send/receive messages.

---

## Message System

### MessageVariant

All messages (system + user) are wrapped in a `std::variant`:

```cpp
using MessageVariant = std::variant<
    down_msg,    // Actor terminated
    exit_msg,    // Exit request
    link_msg,    // Link request
    unlink_msg,  // Unlink request
    // ... user-defined types
>;
```

### Sending Messages

From within an actor:
```cpp
context()->send(target_address, message);
context()->reply(message);           // Reply to sender
context()->reply_with_error(error);   // Reply with error
```

### Linking vs Monitoring

| | Linking | Monitoring |
|---|---|---|
| Scope | Bidirectional | One-way |
| Use case | Death sharing | Watching |
| Effect | Either death affects the other | Only target death is observed |

**Linking:** If actor A links to B, and B dies, A receives `down_msg`. Either actor dying affects the other.

**Monitoring:** If A monitors B, and B dies, A receives `down_msg`. B is unaffected by A's death.

---

## Supervision

Fault-tolerance through hierarchical error handling. When a child actor fails, the parent decides what to do.

### SupervisionPolicy

```cpp
struct SupervisionPolicy {
    enum class Strategy { OneForOne, AllForOne };
    Strategy strategy = OneForOne;
    uint32_t max_restarts = 10;
    std::chrono::milliseconds restart_interval{5000};
};
```

### Strategies

**OneForOne** — Only the failed child is affected:
```
Parent
  ├── Child1 (fails) → Restart Child1 only
  ├── Child2 (continues)
  └── Child3 (continues)
```

**AllForOne** — All children restart when one fails:
```
Parent
  ├── Child1 (fails) → Restart ALL children
  ├── Child2 (restart)
  └── Child3 (restart)
```

### SupervisionDirective

| Directive | Action |
|-----------|--------|
| `Restart` | Restart the failed actor |
| `Stop` | Stop the actor permanently |
| `Escalate` | Pass to this actor's supervisor |

### Supervisor Implementation

- `SupervisorActor` — Delegates to a `Supervisor` strategy object
- `SelfSupervisingActor` — Manages own children with `SupervisionPolicy`, override `on_failure()` for custom logic

---

## ActorAddress

Uniquely identifies an actor across the distributed system:

```cpp
struct ActorAddress {
    EndPoint endpoint;         // IPv4/IPv6 network endpoint
    ActorType type = 0;        // Actor type identifier
    ActorId id;                // Unique instance ID
    uint64_t incarnation = 0;  // Increments on restart
};
```

Location transparency is endpoint-based in the current code. `EndPoint` is a
`std::variant<Ipv4Endpoint, Ipv6Endpoint>` stored directly in `ActorAddress`;
the earlier numeric `NodeId` model has been replaced.

---

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Event-based (not thread-per-actor) | High throughput, no context switching overhead |
| Behavior-based handling | Dynamic behavior switching enables state machines |
| Typed + untyped actors | Type safety where needed, flexibility otherwise |
| Hierarchical supervision | Fault containment, crash recovery |
| No exceptions in hot path | Performance; use `error` class instead |
| No RTTI | Reduced binary size, LLVM conventions |
| Explicit state (StatefulActor) | Clear state ownership, easy inspection |
| Bounded mailbox admission | Prevents mailbox-driven OOM and gives producers backpressure signals |

---

## Current Implementation Boundary

The core runtime is implemented: `ActorSystem::spawn()`, configured topology
spawn, `ActorContext::send()`, `try_send()`, local bounded mailbox admission,
scheduler wakeup, link/monitor delivery, remote actor spawn, async RPC, TCP
transport, service discovery, metrics, CLI inspection, and dead-letter capture
all exist in the codebase.

The remaining production work is not basic runtime wiring; it is the
industry-strength reliability plane described under `docs/architecture/production`.
That backlog covers the still-incomplete contracts for durable delivery,
cluster fencing, graceful drain, remote overload control, sharding, protocol
negotiation, security, and SRE/admin workflows.
