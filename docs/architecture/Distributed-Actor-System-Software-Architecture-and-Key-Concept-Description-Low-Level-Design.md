# Distributed High-Performance Actor System Architecture

**Version:** 1.0  
**Target Implementation:** C++20+  
**Date:** 2026-04-11

---

## 1. Key Concept Definitions

### 1.1 Actor

An actor is the fundamental unit of computation in the actor model.

```cpp
// Conceptual Actor Interface
class Actor {
public:
    // Unique address within the cluster
    ActorId id() const;
    ActorType type() const;

    // Process a single message (turn-based, sequential)
    virtual void receive(Message* msg) = 0;

    // Lifecycle hooks
    virtual void on_activate() {}
    virtual void on_deactivate() {}
    virtual void onCrash(const Error& err) {}
};
```

**Core Properties:**
- **Isolated State:** Each actor maintains private state inaccessible to others
- **Single-threaded Execution:** Only one message processed at a time per actor
- **Mailbox:** Messages queue in a mailbox, processed sequentially
- **Address-based Communication:** Actors communicate via message passing using addresses

### 1.2 Actor Address

An actor's address is distinct from its memory location, enabling location transparency.

```cpp
struct ActorAddress {
    NodeId     node_id;      // Network location
    ActorType  type;        // Actor "class"
    ActorId    id;          // Unique instance identifier
    uint64_t   incarnation; // Handles host restarts
};
```

### 1.3 Actor Type

Defines the "class" of an actor, specifying supported methods and state schema.

```cpp
// Actor type registration
struct ActorTypeDef {
    std::string name;
    std::vector<MethodSignature> methods;
    StateSchema state_schema;
    Config config;  // e.g., idle_timeout, max_queue_depth
};
```

### 1.4 Message

The sole communication primitive between actors.

```cpp
struct Message {
    MessageId       id;           // Unique message ID
    ActorAddress    sender;       // Reply-to address
    MethodName      method;       // Method to invoke
    std::vector<uint8_t> payload; // Serialized parameters
    bool            is_one_way;    // Fire-and-forget
};
```

### 1.5 Turn-Based Concurrency

Each actor instance processes messages one at a time, eliminating internal race conditions.

```cpp
class ActorContext {
    std::queue<Message> mailbox_;
    std::mutex mutex_;  // Protects queue, one per actor
    std::condition_variable cv_;
    bool processing_ = false;
};
```

### 1.6 Actor Lifecycle States

```
                    ┌─────────────┐
                    │  Created    │
                    └──────┬──────┘
                           │ on_activate()
                           ▼
                   ┌─────────────┐
        ┌──────────│   Active    │──────────┐
        │          └──────┬──────┘          │
        │                 │ idle_timeout    │
        │                 ▼                 │
        │          ┌─────────────┐          │
        │          │ Hibernating │          │
        │          └──────┬──────┘          │
        │                 │ invocation      │
        │                 ▼                 │
        │          ┌─────────────┐          │
        └─────────▶│  Deleted    │◀─────────┘
                   └─────────────┘
                          ▲
                          │ host_crash / explicit_deletion
```

### 1.7 Supervision Tree

Hierarchical fault management inspired by Erlang/OTP.

```cpp
class SupervisorStrategy {
    enum class Directive {
        Restart,      // Restart the failed actor
        Stop,         // Terminate permanently
        Escalate,     // Pass to parent supervisor
    };

    virtual Directive on_child_failure(ActorId child, const Error& err) = 0;
};
```

### 1.8 Virtual Actor

An actor that is "always available" - instantiated on-demand by the runtime.

```cpp
// Orleans-style virtual actor
class Grain : public Actor {
    // Runtime handles:
    // - Activation/deactivation
    // - Placement
    // - State persistence
    // - Failure recovery
};
```

---

## 2. Architecture Overview

### 2.1 High-Level Component Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              CLIENT LAYER                                   │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                                   │
│  │  Client  │  │  Client  │  │  Client  │   ...                             │
│  │   SDK    │  │   SDK    │  │   SDK    │                                   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘                                   │
└───────┼─────────────┼─────────────┼─────────────────────────────────────────┘
        │             │             │
        │ gRPC/HTTP   │             │
        ▼             ▼             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           NETWORK LAYER                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                    Message Router / Load Balancer                   │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────────┘
        │                    │                    │
        ▼                    ▼                    ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│   Node 1      │    │   Node 2      │    │   Node N      │
│ ┌───────────┐ │    │ ┌───────────┐ │    │ ┌───────────┐ │
│ │ Actor Host│ │    │ │ Actor Host│ │    │ │ Actor Host│ │
│ │ ┌───────┐ │ │    │ │ ┌───────┐ │ │    │ │ ┌───────┐ │ │
│ │ │Actor1 │ │ │    │ │ │Actor3 │ │ │    │ │ │Actor5 │ │ │
│ │ └───────┘ │ │    │ │ └───────┘ │ │    │ │ └───────┘ │ │
│ │ ┌───────┐ │ │    │ │ ┌───────┐ │ │    │ │ ┌───────┐ │ │
│ │ │Actor2 │ │ │    │ │ │Actor4 │ │ │    │ │ │Actor6 │ │ │
│ │ └───────┘ │ │    │ │ └───────┘ │ │    │ │ └───────┘ │ │
│ └───────────┘ │    │ └───────────┘ │    │ └───────────┘ │
└───────────────┘    └───────────────┘    └───────────────┘
        │                    │                    │
        └────────────────────┼────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                       PLACEMENT SERVICE                                     │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐              │
│  │   Controller    │  │   Controller    │  │   Controller    │              │
│  │   (Leader)      │◄─┤   (Follower)    │  │   (Follower)    │              │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘              │
└───────────┼────────────────────┼────────────────────┼────────────── ────────┘
            │                    │                    │
            └────────────────────┼────────────────────┘
                                 │
                                 ▼
                    ┌─────────────────────────┐
                    │   State Store           │
                    │   (PostgreSQL/Redis)    │
                    └─────────────────────────┘
```

### 2.2 Node Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                          NODE                                  │
│                                                                │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │  Transport   │    │    Actor     │    │    Timer     │      │
│  │   Layer      │    │    Host      │    │   Service    │      │
│  │ (gRPC/HTTP) │◄──▶│  Runtime     │◄──▶│  (Alarms)     │      │
│  └──────┬───────┘    └──────┬───────┘    └──────────────┘      │
│         │                   │                                  │
│         │           ┌───────┴───────┐                          │
│         │           │               │                          │
│         │     ┌─────▼─────┐   ┌─────▼─────┐                    │
│         │     │  Actor    │   │  Actor    │   ...              │
│         │     │  Instance │   │  Instance │                    │
│         │     │  ┌─────┐  │   │  ┌─────┐  │                    │
│         │     │  │Mail │  │   │  │Mail │  │                    │
│         │     │  │box  │  │   │  │box  │  │                    │
│         │     │  └─────┘  │   │  └─────┘  │                    │
│         │     └───────────┘   └───────────┘                    │
│         │                                                      │
│         └───────────────────────────────────────────────────.  ┤
│                                                                │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │    State     │    │   Health     │    │   Config     │      │
│  │   Manager    │    │   Monitor    │    │   Source     │      │
│  └──────────────┘    └──────────────┘    └──────────────┘      │
└────────────────────────────────────────────────────────────────┘
```

---

## 3. System Requirements

### 3.1 Functional Requirements

| ID | Requirement | Description |
|----|-------------|-------------|
| FR-1 | Actor Invocation | Clients can invoke actor methods via type/id |
| FR-2 | Location Transparency | Clients need not know actor's physical location |
| FR-3 | Turn-Based Execution | Each actor processes one message at a time |
| FR-4 | Actor Lifecycle | Support activation, hibernation, deactivation |
| FR-5 | State Persistence | Actor state survives hibernation/failures |
| FR-6 | Supervision | Hierarchical fault recovery (restart strategies) |
| FR-7 | Alarms/Reminders | Time-based scheduled invocations |
| FR-8 | Inter-Actor Communication | Actors can invoke other actors |
| FR-9 | Distributed Deployment | Actors distributed across multiple nodes |
| FR-10 | Actor Type Registration | Runtime support for multiple actor types |

### 3.2 Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Latency | < 1ms local, < 10ms remote invocation |
| NFR-2 | Throughput | > 1M messages/second per node |
| NFR-3 | Scalability | Support 1M+ actors per cluster |
| NFR-4 | Availability | 99.99% uptime with automatic failover |
| NFR-5 | Memory Efficiency | < 1KB overhead per actor |
| NFR-6 | Recovery Time | < 5s for host failure recovery |
| NFR-7 | Backpressure | Graceful degradation under load |

### 3.3 Performance Design Goals

```cpp
// Target performance characteristics
struct PerformanceTargets {
    // Per-actor throughput
    static constexpr size_t messages_per_second = 100'000;

    // Queue depths
    static constexpr size_t max_mailbox_size = 10'000;

    // Latency budgets (microseconds)
    static constexpr uint32_t local_invoke_p99 = 500;    // 0.5ms
    static constexpr uint32_t remote_invoke_p99 = 5'000;  // 5ms

    // Resource limits
    static constexpr size_t max_active_actors_per_node = 100'000;
    static constexpr size_t actor_state_cache_mb = 1024;  // 1GB
};
```

---

## 4. Subsystem Design

### 4.1 Transport Layer

**Responsibility:** Network communication between nodes and clients.

```cpp
// Transport interface
class Transport {
public:
    virtual ~Transport() = default;

    // Start listening for connections
    virtual void listen(const Endpoint& ep) = 0;

    // Send a message (one-way)
    virtual void send(const ActorAddress& to, Message&& msg) = 0;

    // Invoke and wait for response
    virtual Task<Response> invoke(
        const ActorAddress& to,
        const Message& msg,
        std::chrono::milliseconds timeout
    ) = 0;

    // Health check
    virtual bool is_healthy() const = 0;
};

// Concrete implementation using gRPC
class GrpcTransport : public Transport {
    // gRPC channel pool, connection management
    // TLS/mTLS support
    // Circuit breaker patterns
};
```

**Key Features:**
- gRPC for internal communication (high performance, proto support)
- HTTP/1.1 fallback for external clients
- TLS/mTLS for security
- Connection pooling
- Circuit breakers for fault isolation

### 4.2 Actor Host Runtime

**Responsibility:** Execute actors, manage message processing, enforce turn-based concurrency.

```cpp
class ActorHost {
public:
    // Node lifecycle
    void start();
    void stop();

    // Actor management
    ActorId activate_actor(ActorType type, ActorId id);
    void deactivate_actor(ActorId id);

    // Message processing
    void enqueue_message(ActorId target, Message&& msg);

    // Registration
    void register_actor_type(ActorTypeDef def);

private:
    // Per-actor state
    struct ActorInstance {
        std::unique_ptr<Actor> actor;
        ActorContext context;      // Mailbox + concurrency
        uint64_t incarnation;      // Detects restarts
        std::optional<AlarmHandle> pending_alarm;
    };

    std::unordered_map<ActorId, ActorInstance> actors_;
    ThreadPool thread_pool_;  // Work distribution
    // ...
};
```

### 4.3 Placement Service

**Responsibility:** Map actors to nodes, handle distribution and rebalancing.

```cpp
// Placement service interface
class PlacementService {
public:
    // Query where an actor lives
    virtual Task<ActorAddress> find_actor(ActorType type, ActorId id) = 0;

    // Bind actor to a node
    virtual void place_actor(ActorType type, ActorId id, NodeId node) = 0;

    // Remove placement
    virtual void remove_placement(ActorType type, ActorId id) = 0;

    // Rebalance (centralized only)
    virtual Task<rebalance_plan> rebalance() = 0;
};

// Centralized placement with database backend
class ControllerPlacement : public PlacementService {
    // Leader election (Raft/paxos)
    // Placement cache for clients
    // Heartbeat monitoring
    // Host health tracking
};

// Decentralized placement via consistent hashing
class HashPlacement : public PlacementService {
    std::vector<NodeId> nodes_;
    JumpConsistentHash hash_;

    NodeId find_node(ActorType type, ActorId id) override;
};
```

**Placement Strategies:**

| Strategy | Pros | Cons |
|----------|------|------|
| Centralized | Global optimization, easy rebalancing | Single point of failure (mitigated), extra latency |
| Decentralized (Hash) | No coordination, no single failure | Limited rebalancing, membership propagation |

### 4.4 State Management

**Responsibility:** Persist and retrieve actor state.

```cpp
class StateManager {
public:
    // Get actor's persisted state
    virtual Task<State> load(ActorType type, ActorId id) = 0;

    // Persist actor state
    virtual Task<void> save(
        ActorType type,
        ActorId id,
        const State& state,
        ConsistencyLevel level  // strong/eventual
    ) = 0;

    // Delete state (for garbage collection)
    virtual Task<void> delete_state(ActorType type, ActorId id) = 0;
};

// Usage within an actor
class PersistentActor : public Actor {
protected:
    State& state() { return state_; }

    // Convenience methods
    Task<> load_state();
    Task<> save_state();

private:
    State state_;
    StateManager* state_manager_;  // Injected
};
```

**Consistency Considerations:**
- **Strong consistency:** Required for actor state (read-your-own-write)
- **Write-ahead logging:** For crash recovery
- **Serialization:** Protobuf recommended for performance

### 4.5 Timer / Alarm Service

**Responsibility:** Schedule and deliver time-based messages.

```cpp
class AlarmService {
public:
    // Schedule an alarm
    virtual AlarmHandle schedule_alarm(
        ActorType type,
        ActorId id,
        AlarmName name,
        Instant due_time,
        std::optional<Duration> interval = std::nullopt,
        std::optional<size_t> max_repeats = std::nullopt
    ) = 0;

    // Cancel alarm
    virtual void cancel_alarm(AlarmHandle handle) = 0;

    // Replace alarm (for "reset timeout" patterns)
    virtual void upsert_alarm(...) = 0;
};

// Alarm delivery as a message
struct AlarmMessage {
    ActorId target;
    AlarmName alarm_name;
    AlarmTrigger trigger;  // first, repeating, final
};
```

**Alarm Delivery Guarantees:**
- At-least-once delivery
- Idempotent handlers recommended
- Catch-up logic for missed alarms

### 4.6 Supervision System

**Responsibility:** Fault detection and recovery.

```cpp
// Supervision directives
enum class SupervisionDirective {
    Restart,    // Restart the child
    Stop,       // Permanent termination
    Escalate,   // Let parent handle
};

// Supervision strategies
class OneForOneStrategy {
    // Only restart the failed child
};

class AllForOneStrategy {
    // Restart all children if any fails
};

class RestartStrategy {
public:
    virtual SupervisionDirective decide(
        const ChildFailure& failure,
        const SupervisionPolicy& policy
    ) = 0;
};

// Decider implementation
class MyActorSupervisor : public Supervisor {
    SupervisionDirective on_failure(
        ActorId child_id,
        const Error& error
    ) override {
        if (error.is_transient()) {
            return SupervisionDirective::Restart;
        }
        return SupervisionDirective::Stop;
    }
};
```

### 4.7 Client SDK

**Responsibility:** Simplify actor invocation for clients.

```cpp
// C++ client SDK example
class ActorClient {
public:
    ActorClient(const ClusterConfig& config);

    // Typed actor proxy
    template<typename T>
    T get_actor(ActorId id) {
        return T(actor_proxy_, id);
    }

    // Generic invocation
    Task<Response> invoke(
        ActorType type,
        ActorId id,
        MethodName method,
        const google::protobuf::Message& request
    );

    // With placement caching
    Task<Response> invoke_cached(...);
};

// Code-generated actor interface
class ShoppingCartClient {
public:
    ShoppingCartClient(std::shared_ptr<ActorProxy> proxy, ActorId id)
        : proxy_(proxy), id_(id) {}

    Task<GetItemsResponse> GetItems();
    Task<AddItemResponse> AddItem(const AddItemRequest& req);
    Task<CheckoutResponse> Checkout();

private:
    std::shared_ptr<ActorProxy> proxy_;
    ActorId id_;
};
```

---

## 5. Subsystem Boundaries & Interfaces

### 5.1 Subsystem Boundary Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        CLIENTS                                  │
└────────────────────────────┬────────────────────────────────────┘
                             │ invoke(type, id, method, payload)
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                      PUBLIC API                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              ActorClient SDK (C++)                      │    │
│  │              - Type-safe proxies                        │    │
│  │              - Placement caching                        │    │
│  │              - Retry/timeout logic                      │    │
│  └─────────────────────────────────────────────────────────┘    │
└────────────────────────────┬────────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────────┐
│                    TRANSPORT LAYER                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │
│  │  gRPC Server │  │ HTTP Server  │  │  NATS Client │           │
│  └──────────────┘  └──────────────┘  └──────────────┘           │
│  Responsibilities:                                              │
│  - Protocol: gRPC (internal), HTTP (external)                   │
│  - Serialization: Protobuf                                      │
│  - Security: TLS/mTLS                                           │
└────────────────────────────┬────────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────────┐
│                     PLACEMENT LAYER                             │
│  ┌─────────────────────┐    ┌─────────────────────┐             │
│  │  Placement Client   │    │  Placement Service  │             │
│  │  (caches routing)   │◄──▶│  (controller/etc)   │             │
│  └─────────────────────┘    └─────────────────────┘             │
│  Responsibilities:                                              │
│  - Actor-to-node mapping                                        │
│  - Placement caching with TTL                                   │
│  - Controller discovery                                         │
└────────────────────────────┬────────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────────┐
│                     ACTOR HOST RUNTIME                          │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐              │
│  │   Actor     │  │   Actor     │  │   Actor     │              │
│  │  Instance   │  │  Instance   │  │  Instance   │              │
│  │  (Mailbox)  │  │  (Mailbox)  │  │  (Mailbox)  │              │
│  └─────────────┘  └─────────────┘  └─────────────┘              │
│  Responsibilities:                                              │
│  - Turn-based message processing                                │
│  - Actor lifecycle management                                   │
│  - Thread pool work distribution                                │
└────────────────────────────┬────────────────────────────────────┘
                             │
          ┌──────────────────┼──────────────────┐
          │                  │                  │
          ▼                  ▼                  ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│  STATE SERVICE  │ │  ALARM SERVICE  │ │SUPERVISION TREE │
│  (Persistence)  │ │   (Timers)      │ │  (Fault TOL.)   │
└─────────────────┘ └─────────────────┘ └─────────────────┘
```

### 5.2 Key Internal Interfaces

```cpp
// Between Placement and Transport
class IPlacementClient {
    Task<ActorAddress> resolve(ActorType type, ActorId id);
    void invalidate_cache(ActorType type, ActorId id);
};

// Between ActorHost and StateManager
class IActorStateStore {
    virtual Task<bytes> load(ActorType, ActorId) = 0;
    virtual Task<> save(ActorType, ActorId, const bytes&) = 0;
    virtual Task<> delete(ActorType, ActorId) = 0;
};

// Between ActorHost and AlarmService
class IAlarmScheduler {
    virtual AlarmHandle schedule(
        ActorId, AlarmName, Instant,
        std::optional<Duration> interval
    ) = 0;
    virtual void cancel(AlarmHandle) = 0;
    virtual void reschedule(AlarmHandle, Instant new_time) = 0;
};

// Between ActorHost and Supervision
class ISupervisor {
    virtual void report_failure(ActorId child, const Error&) = 0;
    virtual void child_stopped(ActorId child) = 0;
};
```

---

## 6. Data Flow

### 6.1 Actor Invocation Flow

```
Client                          Node A                          Placement
  │                               │                                │
  │ invoke(ShoppingCart, u-123,   │                                │
  │         AddItem, {...})       │                                │
  │──────────────────────────────▶│                                │
  │                               │                                │
  │                               │ find_actor(ShoppingCart,       │
  │                               │            u-123)              │
  │                               │───────────────────────────────▶│
  │                               │                                │
  │                               │     ActorAddress{              │
  │                               │       node: NodeB,             │
  │                               │       actor_id: u-123}         │
  │                               │◀───────────────────────────────│
  │                               │                                │
  │                               │ forward(Message{               │
  │                               │   method: AddItem,             │
  │                               │   payload: {...}})             │
  │                               │────────────── ┐                │
  │                               │               │                │
  │                               │               │ (gRPC/net)
  │                               │               ▼                │
  │                               │         ┌─────────────┐        │
  │                               │         │   Node B    │        │
  │                               │         │ ActorHost   │        │
  │                               │         └──────┬──────┘        │
  │                               │                │               │
  │                               │                ▼               │
  │                               │         ┌─────────────┐        │
  │                               │         │ ShoppingCart│        │
  │                               │         │ Actor       │        │
  │                               │         │ (Mailbox)   │        │
  │                               │         └─────────────┘        │
  │                               │                                │
  │ Response{success, new_state}  │                                │
  │◀──────────────────────────────│                                │
```

### 6.2 Actor-to-Actor Communication

```
Actor: Order                    Actor: Inventory
    │                                │
    │ invoke(Inventory,              │
    │         ReserveItems,          │
    │         {sku: "X", qty: 5})    │
    │──────────┐                     │
    │          │ (within same node:  │
    │          │  direct queue       │
    │          │  enqueue)           │
    │          ▼                     │
    │    ┌─────────────┐             │
    │    │  Inventory   │            │
    │    │  Mailbox     │─────────────▶
    │    └─────────────┘             │
    │                                │
    │◀────────────────────────────── │
    │ Response{reserved: true}       │
```

### 6.3 Hibernation and Recovery

```
Normal Operation:
  OrderActor(u-456) ──active──▶ Processing messages

Idle Timeout:
  OrderActor(u-456) ──hibernate──▶ save_state() → DB
                                          │
                                          ▼
                                    Delete in-memory state
                                    Cancel alarm

Invocation after idleness:
  invoke(Order, u-456, GetStatus)
        │
        ▼
  load_state() ← DB
        │
        ▼
  activate OrderActor(u-456)
        │
        ▼
  Process message with restored state
```

---

## 7. Failure Handling

### 7.1 Failure Modes

| Failure | Detection | Recovery |
|---------|-----------|----------|
| Actor host crash | Heartbeat timeout | Placement updated, actor reactivated on new node |
| Network partition | Transport failure | Retry with backoff, circuit breaker |
| Controller failure | Leader election | New leader elected, cached placements invalidated |
| Actor crash | Supervision | Restart per strategy (one-for-one/all-for-one) |
| State store unavailable | Read/write failure | Retry, fail-fast to client |
| Message timeout | Client-side | Retry with idempotency key |

### 7.2 Circuit Breaker

```cpp
class CircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    void record_success();
    void record_failure();

    bool allow_request() {
        std::lock_guard lock(mutex_);
        switch (state_) {
            case State::Closed: return true;
            case State::Open:
                if (elapsed() > reset_timeout_) {
                    state_ = State::HalfOpen;
                    return true;
                }
                return false;
            case State::HalfOpen:
                return true;  // Allow one test request
        }
    }

private:
    State state_ = State::Closed;
    size_t failure_count_ = 0;
    static constexpr size_t threshold_ = 5;
    std::chrono::milliseconds reset_timeout_{30'000};
};
```

---

## 8. Observability

### 8.1 Metrics

```cpp
// Key metrics to expose
struct ActorMetrics {
    // Per-actor
    Counter   messages_processed;
    Histogram message_processing_duration;
    Gauge     mailbox_size;

    // Per-node
    Gauge     active_actors;
    Gauge     hibernated_actors;
    Counter   activations_total;
    Counter   deactivations_total;

    // Cluster-wide
    Counter   placement_cache_hits;
    Counter   placement_cache_misses;
    Histogram actor_invocation_duration;
};
```

### 8.2 Distributed Tracing

Integrate with OpenTelemetry for end-to-end request tracing:

```cpp
// Trace context propagation
struct TraceContext {
    trace_id    trace_id;
    span_id     span_id;
    trace_flags flags;
};

// Inject into message headers
Message with_trace(Message msg, const TraceContext& ctx) {
    msg.headers["trace-id"] = ctx.trace_id;
    msg.headers["span-id"] = ctx.span_id;
    return msg;
}
```

### 8.3 Logging

```cpp
// Structured logging with trace correlation
logger.Info("Actor invoked",
    "actor_type"_a = type,
    "actor_id"_a = id,
    "method"_a = method,
    "trace_id"_a = trace_ctx.trace_id,
    "duration_us"_a = elapsed.count()
);
```

---

## 9. Implementation Considerations

### 9.1 Memory Management

- Use memory pools for actor instances (avoid malloc per message)
- Bounded mailboxes to prevent OOM
- Actor state cache with LRU eviction

```cpp
class ActorPool {
    std::pmr::memory_pool<> pool_;  // Per-type pool
    std::vector<std::unique_ptr<Actor>> recycle_bin_;
    size_t max_size_;
};
```

### 9.2 Thread Model

```cpp
// Hybrid threading: thread-per-core + work stealing
class ThreadPool {
    std::vector<std::thread> workers_;
    WorkStealingQueue<Task> queues_;  // Per-thread queue
    atomic<bool> shutdown_;

    // Actor messages posted to owner's queue
    // Cross-node messages steal from local pool first
};
```

### 9.3 Serialization Strategy

```cpp
// Protobuf for all wire messages
message ActorMessage {
    string actor_type = 1;
    string actor_id = 2;
    string method = 3;
    bytes payload = 4;
    TraceContext trace = 5;
    bool one_way = 6;
}

// Use arena allocation for deserialization
// Protobuf arena for zero-copy when possible
```

### 9.4 Hot Code Loading

For zero-downtime upgrades:

```cpp
// Versioned actor types
struct VersionedType {
    ActorType base_type;
    uint32_t version;
};

// Lazy migration on activation
void MyActor::migrate_state(State& old_state) {
    // Transform old format to new format
    state_.field_a = oldState.field_b;  // Renamed
    state_.nested = flatten(oldState.nested);  // Restructured
}
```

---

## 10. Summary

This architecture provides:

1. **Scalability:** Horizontal scaling via placement service, supports millions of actors
2. **Performance:** Turn-based concurrency eliminates locks, memory pools minimize allocation
3. **Reliability:** Supervision trees for fault recovery, persistent state for durability
4. **Location Transparency:** Clients invoke by type/id, runtime handles routing
5. **Flexibility:** Pluggable placement strategies, state stores, transport layers

**Key Design Decisions:**
- Virtual actor model (on-demand activation) for elastic scalability
- Centralized placement with caching (balances optimization vs. latency)
- Turn-based concurrency per actor (eliminates internal race conditions)
- Hierarchical supervision (fault isolation and recovery)
- Protobuf serialization (performance + cross-language support)

---

*Document generated from analysis of:*
- *Actors & Actor Systems as massively distributed scalability architecture (Volodymyr Pavlyshyn)*
- *Notes on Designing a Distributed Actor Framework (withblue.ink)*
- *An Actor, a model and an architect walk onto the web (surma.dev)*
