# HPActor Actor Routing — Router Subsystem Design

## 1. Executive Summary

The router subsystem provides workload distribution across pools of actors,
filling gap #6 from the Akka Typed Actors gap analysis (issue #329). Routers
intercept messages and forward them to one or more **routees** using a
pluggable routing strategy. Two router types cover the primary use cases:
**PoolRouter** (owns and supervises its routees as children) and
**GroupRouter** (routes to externally-registered actors discovered by service
key).

**Key Design Decisions:**

- **Router is an actor.** Both router types are `EventBasedActor` subclasses.
  A router has a mailbox, a behavior, an address, and participates in normal
  message-passing. Senders do not know they are talking to a router.
- **Pluggable routing logic.** Four strategies (round-robin, random,
  consistent-hashing, smallest-mailbox) implement `IRoutingLogic`. Strategies
  are runtime-swappable.
- **Pool routers own and supervise routees.** `PoolRouter` extends
  `SelfSupervisingActor` — routees are children with full restart counting,
  failure escalation, and quarantine support.
- **Group routers reference external routees.** `GroupRouter` extends
  `EventBasedActor` — routees are discovered/registered actors that exist
  independently. No supervision is applied.
- **No RTTI.** The `on_routees_changed()` virtual hook on `IRoutingLogic`
  avoids `dynamic_cast` for hash-ring rebuilds.
- **Deterministic testing.** All tests use `SchedulerTestDriver` without
  threads or wall-clock sleeps.

---

## 2. Architecture

```
                        IRoutingLogic (abstract)
                        /      |        |        \
           RoundRobinLogic  RandomLogic  ConsHash  SmallestMbox
                       (on_routees_changed virtual hook)

    SelfSupervisingActor                  EventBasedActor
            |                                  |
       PoolRouter                          GroupRouter
      (routees = children,               (routees = external refs,
       supervised restarts)               service key discovery)
```

**Message flow through a router:**

```
Sender ──send(msg)──► Router ──select_routee()──► Routee ──process──► reply ──► Router
```

For fire-and-forget messages, the routee sees the router as the sender.
`context()->reply()` from a routee returns to the router. The existing
`ask()` pattern with `AskManager` handles request-response correlation.

---

## 3. IRoutingLogic — Pluggable Strategies

### 3.1 Interface

```cpp
class IRoutingLogic {
public:
    virtual ~IRoutingLogic() = default;

    /// Select a routee index. Returns 0 when routees is empty.
    virtual size_t select_routee(
        const std::vector<ActorRef>& routees,
        const TypedMessage& msg,
        const std::vector<cli::MboxSnapshot>& routee_states) = 0;

    /// Human-readable name for CLI/logging.
    virtual const char* name() const = 0;

    /// Called when the routee set changes (add, remove, resize).
    /// Override to rebuild internal state (e.g., hash ring).
    virtual void on_routees_changed(
        const std::vector<ActorRef>& routees) {}
};
```

### 3.2 RoundRobinLogic

Sequential routee selection via atomic counter. Each `select_routee()` call
increments and returns `counter % routees.size()`.

- **State:** `std::atomic<uint64_t> counter_`
- **Complexity:** O(1)
- **Use case:** Uniform distribution, stateless workloads.

### 3.3 RandomLogic

Pseudo-random selection using xorshift64. CAS-based state update for
lock-free operation on a single scheduler thread.

- **State:** `std::atomic<uint64_t> state_`
- **Seed:** Constructor accepts `uint64_t seed`. Seed=0 uses a hash of
  `this` for non-deterministic seeding. Same seed → reproducible sequence.
- **Complexity:** O(1)
- **Use case:** Uniform distribution when strict round-robin ordering is
  undesirable (e.g., avoiding correlated failures).

### 3.4 ConsistentHashingLogic

Hash-ring-based selection with configurable virtual nodes. Messages with
the same key always map to the same routee (modulo ring changes).

- **Ring:** Sorted `vector<pair<uint64_t, routee_index>>` with 128 vnodes
  per routee by default.
- **Lookup:** `std::lower_bound` — O(log n).
- **Key extraction:** Default key extractor hashes `TypeTag`. Custom
  extractors can hash on specific message fields (e.g., user ID) for
  domain-based affinity.
- **Rebuild:** `rebuild_ring()` called via `on_routees_changed()` when the
  routee set changes.
- **Use case:** Sticky sessions, cache affinity, sharded state.

### 3.5 SmallestMailboxLogic

Load-aware selection — picks the routee with the smallest
`MboxSnapshot::depth`. Routees without a snapshot (remote, unavailable)
are treated as depth=0.

- **Complexity:** O(n) — scans all routee snapshots.
- **Use case:** Work-stealing, load balancing, heterogeneous workloads.

---

## 4. PoolRouter

### 4.1 Design

`PoolRouter` extends `SelfSupervisingActor` to inherit child lifecycle
management. Routees are spawned as children during `on_activate()` (after
`ActorContext` is fully set up). The behavior intercepts all incoming
messages and forwards to the routee selected by the current
`IRoutingLogic`.

```
SelfSupervisingActor
├── children_          ← routee children (Actor)
├── restart_counts_     ← per-routee restart tracking
├── policy_             ← SupervisionPolicy (max_restarts, interval)
├── handle_child_down() ← receives DownMsg from failed routees
└── on_failure()        ← overridden to replace failed routee
```

### 4.2 API

```cpp
class PoolRouter final : public SelfSupervisingActor {
public:
    PoolRouter(ActorContext* ctx, ActorSystem& sys,
               std::unique_ptr<IRoutingLogic> logic,
               config::ActorFactory factory,
               size_t pool_size,
               SupervisionPolicy policy = {});

    // Routee management
    void add_routee();
    void remove_routee();
    void resize(size_t new_size);
    size_t routee_count() const;

    // Broadcast
    void broadcast(TypedMessage msg);

    // Runtime reconfiguration
    void set_routing_logic(std::unique_ptr<IRoutingLogic> logic);

protected:
    void on_activate() override;
    Behavior make_behavior() override;
    SupervisionDirective on_failure(ActorId child_id,
                                     const error& err) override;
};
```

### 4.3 Routee Lifecycle

**Spawn:** Routees are created via the type-erased `ActorFactory` and
registered with the actor system through `spawn_configured()` — ensuring
each routee gets a mailbox, context, and scheduler registration.

**Failure:** When a routee fails, `SelfSupervisingActor::handle_child_down()`
receives the `DownMsg`. `decide_restart()` checks restart count against
policy limits. `on_failure()` spawns a replacement from the factory and
swaps it into the routee list.

**Resize:** `resize(n)` scales up by spawning new routees or scales down by
removing the last N routees (and their supervision tracking).

### 4.4 Usage Example

```cpp
auto router = system.spawn<PoolRouter>(
    std::make_unique<RoundRobinLogic>(),
    [](ActorContext* ctx, ActorSystem& sys) {
        return std::make_shared<MyWorkerActor>(ctx, sys);
    },
    5  // pool size
);

context()->send(router.address(), TypedMessage(tag, payload));
router->broadcast(TypedMessage(bcast_tag, payload));
router->resize(10);  // scale up
```

---

## 5. GroupRouter

### 5.1 Design

`GroupRouter` extends `EventBasedActor`. Routees are externally-registered
actor references — they are NOT children and receive no supervision from
the router. Routees are identified by a service key string for integration
with service discovery / Receptionist patterns.

### 5.2 API

```cpp
class GroupRouter final : public EventBasedActor {
public:
    GroupRouter(ActorContext* ctx, ActorSystem& sys,
                std::unique_ptr<IRoutingLogic> logic,
                std::string service_key);

    // Routee management
    void add_routee(ActorRef routee);
    void remove_routee(const ActorAddress& addr);
    void set_routees(std::vector<ActorRef> routees);
    size_t routee_count() const;

    // Service key
    const std::string& service_key() const;

    // Broadcast
    void broadcast(TypedMessage msg);

    // Runtime reconfiguration
    void set_routing_logic(std::unique_ptr<IRoutingLogic> logic);

protected:
    Behavior make_behavior() override;
};
```

### 5.3 Usage Example

```cpp
auto router = system.spawn<GroupRouter>(
    std::make_unique<ConsistentHashingLogic>(),
    "image-processor"  // service key
);

// Routees discovered externally
router->add_routee(ActorRef(some_actor));
router->add_routee(ActorRef(another_actor));

context()->send(router.address(), TypedMessage(tag, payload));
router->broadcast(TypedMessage(bcast_tag, payload));
```

---

## 6. Broadcast

Both router types provide `broadcast(TypedMessage msg)` which sends a
copy of the message to every routee. The `TypedMessage` copy constructor
is deleted, so broadcast reconstructs each copy from the message's
`TypeTag` and a copy of the `StreamBuffer` payload.

---

## 7. Design Rationale

### 7.1 Why Router is an Actor

An actor router participates in normal message-passing, has its own
address, and can be supervised. This means:
- Senders use the same `context()->send()` API regardless of whether the
  target is a single actor or a router.
- Routers can be spawned anywhere in the supervision tree.
- The router's mailbox provides natural backpressure — if all routees are
  busy, messages queue at the router.

### 7.2 Why PoolRouter Extends SelfSupervisingActor

Routees managed by a pool are children of the pool. When a routee fails,
the pool should decide whether to restart, stop, or quarantine it — exactly
the responsibility of a supervisor. By extending `SelfSupervisingActor`,
`PoolRouter` inherits restart counting, sliding window policy enforcement,
and quarantine escalation without duplicating logic.

### 7.3 Why Separate PoolRouter and GroupRouter

Pool and group routers have different lifecycle ownership models. A pool
owns its routees (creates, supervises, destroys). A group references
external routees that exist independently. Combining both into a single
class would require runtime mode switching and conditional supervision
logic, increasing complexity and test surface.

### 7.4 Why on_routees_changed() Instead of dynamic_cast

`-fno-rtti` prohibits `dynamic_cast`. The `on_routees_changed()` virtual
hook on `IRoutingLogic` lets `ConsistentHashingLogic` rebuild its hash ring
when routees change, without the router knowing which strategy is active.
`RoundRobinLogic`, `RandomLogic`, and `SmallestMailboxLogic` leave it as a
no-op.

### 7.5 Message Forwarding Semantics

The router forwards messages via `context()->send()`. The routee sees the
router as the sender — `context()->reply()` returns to the router.
For v1, the router does not transparently relay replies to the original
sender. Use the `ask()` pattern with `AskManager` for request-response
correlation.

---

## 8. Edge Cases

| Scenario | Behavior |
|----------|----------|
| Empty routee list | `select_routee()` returns 0; router drops message |
| Single routee | All strategies return 0 (correct degeneracy) |
| Remote routee (GroupRouter) | `ActorProxy` handles remote dispatch; mailbox snapshot is zeroed |
| Consistent hash ring empty | `select_routee()` returns 0 |
| Routee restart exceeds max_restarts | Supervisor issues Stop or Quarantine per policy |
| Concurrent resize + routing | All on same actor thread — no synchronization needed |

---

## 9. Testing

All 36 tests use `SchedulerTestDriver` for deterministic execution:

| Test file | Tests | Coverage |
|-----------|-------|----------|
| `test_routing_logic.cpp` | 19 | RoundRobin (4), Random (4), ConsistentHash (5), SmallestMailbox (5), on_routees_changed (1) |
| `test_pool_router.cpp` | 10 | Spawn, routing, broadcast, resize (up/down), add/remove, failure restart, logic swap |
| `test_group_router.cpp` | 7 | Empty, add, remove, set, broadcast, service key, logic swap |

---

## 10. File Layout

```
include/hpactor/actor/routing/
├── routing_logic.hpp        # IRoutingLogic + 4 strategies
├── pool_router.hpp           # PoolRouter class
└── group_router.hpp          # GroupRouter class

src/actor/routing/
├── routing_logic.cpp         # Strategy implementations
├── pool_router.cpp           # PoolRouter implementation
└── group_router.cpp          # GroupRouter implementation

tests/unit/actor/routing/
├── test_routing_logic.cpp    # 19 tests
├── test_pool_router.cpp      # 10 tests
└── test_group_router.cpp     # 7 tests
```

---

## 11. Future Enhancements

| Feature | Description |
|---------|-------------|
| Transparent reply routing | Routee's `reply()` goes to original sender, not router |
| TOML router config | `[system.router.<name>]` sections for declarative topology |
| Broadcast as routing strategy | `BroadcastLogic` returns all indices; router sends to all |
| Router metrics | Routee utilization, message distribution histogram |
| Weighted routing | Weighted random/round-robin for heterogeneous routees |
| Adaptive smallest-mailbox | Incorporate processing latency, not just depth |

---

## 12. References

- [Actors Data Structure Design](actors-data-structure-design.md)
- [User-Defined Actor Programming Model](user-defined-actor-programming-model.md)
- [Supervision](actors-data-structure-design.md#9-supervision)
- [Issue #329 — Akka Gap Analysis](https://github.com/skg7on/HPActor/issues/329)
- [Akka Routers Documentation](https://doc.akka.io/libraries/akka-core/current/typed/routers.html)
