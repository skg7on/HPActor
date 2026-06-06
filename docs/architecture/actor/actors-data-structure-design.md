# C++ Actor Framework - Actor Core Concept and Design Specification

## 1. Executive Summary

This document specifies the Actor implementation for the HPActor C++20 framework. Actors follow an **event-based** programming model with **turn-based concurrency**, supporting both **statically typed** and **dynamically typed** messaging. The design is inspired by CAF (C++ Actor Framework) and aligns with the distributed system architecture.

**Key Design Decisions:**
- Event-based actors with cooperative scheduling (M:N work-stealing)
- Pinned dispatcher for daemon, I/O, and compute-heavy actors — isolated at the scheduling layer while remaining in the supervision tree
- Explicit lifecycle with passivation for idle actors
- Durable actor state with snapshot/event-sourcing and schema migration
- Typed and dynamically typed actor support
- Hierarchical supervision (OneForOne, AllForOne)
- Actor references: local handles + distributed addresses

---

## 2. Dispatch Policy — Separating Actor Kind from Execution

A fundamental design choice: _what_ an actor is (its type, behavior, place in the supervision tree) is orthogonal to _how_ it is scheduled. The `DispatchPolicy` bridges the two, telling the scheduler which execution model to use for a given actor.

```cpp
// DispatchPolicy - tells the scheduler how to execute this actor
enum class DispatchPolicy : uint8_t {
    // Default: cooperatively scheduled on the M:N work-stealing pool.
    // Suitable for short, non-blocking, turn-based handlers.
    Cooperative = 0,

    // Pinned to a dedicated OS thread. The actor's receive/run loop blocks
    // on I/O or polls a resource indefinitely. Does NOT consume a worker
    // from the M:N pool — it gets its own std::thread.
    // Suitable for: daemons, DPDK pollers, epoll/io_uring event loops.
    DedicatedThread,

    // Pinned to a dedicated worker pool (1+ threads). The actor still
    // participates in message-passing, but its handlers are dispatched
    // from a private thread pool so that long-running compute never
    // starves the cooperative workers.
    // Suitable for: dense computation, ML inference, image processing.
    DedicatedPool,
};

// Hints optionally attached to a policy for scheduler tuning
struct DispatchHints {
    // For DedicatedThread: pin to a specific CPU core (-1 = no affinity)
    int cpu_affinity = -1;

    // For DedicatedPool: number of threads in the private pool (default 1)
    uint32_t pool_size = 1;

    // Scheduling priority class within the policy domain
    uint8_t priority = 0;
};
```

**Benefits of policy-as-attribute (not inheritance):**

| Concern | Where it lives |
|---------|---------------|
| Actor identity, messaging, supervision | Actor class hierarchy |
| How the actor is scheduled | `DispatchPolicy` + `DispatchHints` |
| Lifecycle (activate/hibernate/die) | `ActorLifecycle` state machine |

A `DenseComputingActor` _is_ an `EventBasedActor` — it has a mailbox, a behavior, and a supervisor. The only difference is that its `DispatchPolicy` is `DedicatedPool`, so the scheduler never queues it on the cooperative workers. Same for a `DaemonActor`: it inherits from `EventBasedActor` but carries `DedicatedThread` so it gets its own OS thread.

---

## 3. Actor Type Hierarchy (Refined)

```
                              ┌──────────────────────────────────────┐
                              │          AbstractActor               │
                              │  - id(), type(), address()           │
                              │  - system(), link_to(), monitor()    │
                              │  - receive(TypedMessage&) = 0        │
                              │  - dispatch_policy() const = 0       │
                              │  - dispatch_hints() const            │
                              └──────────────────┬───────────────────┘
                                                 │
                         ┌───────────────────────┼───────────────────────┐
                         ▼                       ▼                       ▼
              ┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
              │   LocalActor     │    │  (future)        │    │  (future)        │
              │  - context()     │    │  RemoteActor     │    │  VirtualActor    │
              │  - home_system() │    │  (placeholder)   │    │  (Orleans-style) │
              └────────┬─────────┘    └──────────────────┘    └──────────────────┘
                       │
         ┌─────────────┼──────────────────────────┐
         ▼             ▼              ▼           ▼
  ┌──────────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐
  │ EventBased   │ │ Blocking │ │ Typed    │ │ ScopedActor  │
  │ Actor        │ │ Actor    │ │ Actor<>  │ │ (non-actor   │
  │ (cooperative)│ │(threaded)│ │ (static) │ │  contexts)   │
  └──────┬───────┘ └────┬─────┘ └─────┬────┘ └──────────────┘
         │              │             │
         │    ┌─────────┘             │
         ▼    ▼                       ▼
  ┌──────────────┐         ┌──────────────────────┐
  │ Stateful<T>  │         │ SupervisorActor      │
  │              │         │ SelfSupervisingActor │
  └──────────────┘         └──────────────────────┘

  Specialization via DispatchPolicy (NOT inheritance):

  EventBasedActor + DedicatedThread  ──▶  DaemonActor (conceptual)
      │
      ├──▶ PollingActor (DPDK, periodic resource poll)
      │
      └──▶ ExternalMsgGatewayActor (HTTP, gRPC ingress)

  EventBasedActor + DedicatedPool    ──▶  DenseComputingActor (conceptual)

  ExternalMsgGatewayActor ──▶ HTTPServerActor
```

### 3.1 Why Policy, Not Inheritance, for Dispatch

A `DaemonActor` **is** an `EventBasedActor`. It participates in message-passing, has a `Behavior`, is supervised, and can spawn children. The only thing different is _where its `receive()` runs_. Encoding that as a `DispatchPolicy` on the base class means:

1. **Supervision is unaffected.** The daemon is a child in the tree; a `SupervisorActor` restarts it on failure without knowing it was pinned.
2. **Messaging is unaffected.** Other actors `send()` to it exactly as they would to any other actor.
3. **The scheduler is the only component that branches on policy.** When the scheduler dequeues work for this actor, it checks `dispatch_policy()`:
   - `Cooperative` → run on the current worker thread.
   - `DedicatedThread` → the actor has its own thread; the scheduler just ensures the mailbox is delivered and the dedicated thread is woken if blocked.
   - `DedicatedPool` → enqueue to the actor's private thread pool instead of the global work-stealing pool.

---

## 4. Fundamental Types

Core types used throughout the actor framework. These must be defined before any actor components.

```cpp
// ActorId - unique identifier for an actor instance
struct ActorId {
    using counter_type = uint64_t;

    ActorId() = default;
    explicit ActorId(counter_type value) : value_(value) {}

    counter_type value() const { return value_; }
    bool operator==(const ActorId& other) const { return value_ == other.value_; }

private:
    counter_type value_ = 0;
};

// NodeId - identifier for a node in the distributed system
using NodeId = uint32_t;
constexpr NodeId InvalidNodeId = 0;

// ActorType - identifies the "class" of an actor
using ActorType = uint32_t;
constexpr ActorType InvalidActorType = 0;

// Incarnation - incremented on actor restart to detect stale references
using incarnation_type = uint64_t;

// MessageId - unique message identifier
struct MessageId {
    uint64_t value;
    static MessageId generate();
};

// error - error code wrapper (no exceptions in hot path)
class error {
public:
    error() = default;
    explicit error(uint32_t code, std::string msg = {})
        : code_(code), message_(std::move(msg)) {}

    uint32_t code() const { return code_; }
    const std::string& message() const { return message_; }
    bool ok() const { return code_ == 0; }
    explicit operator bool() const { return !ok(); }

private:
    uint32_t code_ = 0;
    std::string message_;
};

// Common error codes
namespace errors {
constexpr uint32_t unknown = 1;
constexpr uint32_t actor_down = 2;
constexpr uint32_t actor_not_found = 3;
constexpr uint32_t mailbox_full = 4;
constexpr uint32_t timeout = 5;
constexpr uint32_t user = 1000;  // Start of user-defined codes
} // namespace errors

// Clock - for time-based operations
class Clock {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::milliseconds;

    time_point now() const { return time_point::clock::now(); }
};

// AlarmHandle - opaque handle for scheduled alarms
class AlarmHandle {
public:
    AlarmHandle() = default;
    explicit AlarmHandle(uint64_t id) : id_(id) {}
    uint64_t id() const { return id_; }

private:
    uint64_t id_ = 0;
};

// TraceContext - for distributed tracing (OpenTelemetry)
struct TraceContext {
    uint64_t trace_id = 0;
    uint64_t span_id = 0;
    uint8_t flags = 0;
};

// bytes - raw byte buffer for serialization
using bytes = std::vector<uint8_t>;

// Task<T> - coroutine-based async task (defined in task.hpp)
template<typename T>
class Task;

// Task<> specialization for void
template<>
class Task<void>;
```

---

## 5. Base Actor Classes

### 5.1 abstract_actor

Base class for all actors. Provides identity, linking, and monitoring. Extended with dispatch policy support.

```cpp
class abstract_actor : public std::enable_shared_from_this<abstract_actor> {
public:
    virtual ~abstract_actor() = default;

    ActorId id() const { return id_; }
    ActorAddress address() const;
    ActorSystem& system() { return system_; }

    // Dispatch policy — tells the scheduler how to execute this actor
    virtual DispatchPolicy dispatch_policy() const { return DispatchPolicy::Cooperative; }
    virtual DispatchHints dispatch_hints() const { return {}; }

    // Lifetime hooks (called by ActorHost)
    virtual void on_activate() {}
    virtual void on_deactivate() {}

    // Linking - death sharing
    void link_to(ActorAddr other);
    void unlink_from(ActorAddr other);

    // Monitoring - receive down messages
    void monitor(const ActorAddr& target);
    void demonitor(const ActorAddr& target);

    // Spawn children from context (C++20 uses template, not auto params)
    template<typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);

    // Receive message (called by scheduler)
    virtual void receive(MessageVariant&& msg) = 0;

protected:
    abstract_actor(ActorId id, ActorSystem& sys);

    // Overridden by LocalActor to return the ActorContext.
    virtual ActorContext* actor_context() { return nullptr; }

private:
    ActorId id_;
    ActorType type_;
    ActorSystem& system_;
    ActorAddress address_;
};
```

### 5.2 local_actor

Base for locally executed actors. Provides access to ActorContext.

```cpp
class local_actor : public abstract_actor {
public:
    ActorContext* context() { return ctx_; }
    ActorSystem& home_system() { return system(); }

protected:
    local_actor(ActorContext* ctx, ActorSystem& sys);

private:
    ActorContext* ctx_;
};
```

### 5.3 event_based_actor

Cooperatively scheduled actor with behavior-based message handling. This is the default actor type for most workloads.

```cpp
class event_based_actor : public local_actor {
public:
    void become(Behavior bh);
    void become_empty();

    void receive(MessageVariant&& msg) override;

protected:
    virtual Behavior make_behavior() { return {}; }
    void on_activate() override;
    void on_deactivate() override;
    virtual void on_exit() {}

private:
    Behavior behavior_;
};
```

### 5.4 typed_event_based_actor

Statically typed actor (CAF-style). Compile-time message type verification.

```cpp
template<typename... Signatures>
class typed_event_based_actor : public local_actor {
public:
    using behavior_type = typed_behavior<Signatures...>;

    void become(behavior_type bh);

    template<typename T>
    typename handler_type<T>::result operator()(T&& msg);

protected:
    virtual behavior_type make_behavior() = 0;

private:
    behavior_type behavior_;
};
```

### 5.5 stateful_actor

Actor with explicit state managed via a state class.

```cpp
template<typename T>
class stateful_actor : public event_based_actor {
public:
    T& state() { return state_; }
    const T& state() const { return state_; }

protected:
    virtual Behavior make_behavior() = 0;

private:
    T state_;
};
```

### 5.6 blocking_actor

Actor running in its own thread with blocking receive. By default uses `DedicatedThread` dispatch policy.

```cpp
class blocking_actor : public local_actor {
public:
    DispatchPolicy dispatch_policy() const override { return DispatchPolicy::DedicatedThread; }

    template<typename... Handlers>
    void receive(Handlers&&... handlers);

    template<typename T>
    void receive_for(T& begin, T end);

    template<typename... Actors>
    void wait_for(ActorAddr first, Actors&&... rest);

    void await_all_other_actors_done();

    const error& fail_state() const { return fail_state_; }
    void fail_state(error e) { fail_state_ = e; }

protected:
    void on_activate() override;
    void on_deactivate() override;

private:
    error fail_state_;
};
```

### 5.7 scoped_actor

Blocking actor for non-actor contexts (e.g., `main()`).

```cpp
class scoped_actor : public blocking_actor {
public:
    explicit scoped_actor(ActorSystem& sys);
    ~scoped_actor();

    template<typename T>
    T receive();
};
```

---

## 6. Special Actors with Pinned Dispatch

### 6.1 Design Rationale

Systems handle heterogeneous workloads:

| Workload | Challenge | Solution |
|----------|-----------|----------|
| Heavy I/O (DB, files, sockets) | Blocks worker threads | `DedicatedThread` — own OS thread |
| DPDK packet polling | Busy-poll, 100% CPU on a core | `DedicatedThread` with CPU affinity |
| Dense computation (ML, crypto) | Long-running, starves M:N pool | `DedicatedPool` — private thread pool |
| External message gateways (HTTP, gRPC) | Own event loop (epoll/io_uring) | `DedicatedThread` — own reactor |

All of these remain **actors**. They have addresses, mailboxes, behaviors, and supervisors. The only thing that changes is _where their execution runs_.

### 6.2 DaemonActor

Base for actors that own a long-running, potentially blocking event loop. The actor's dedicated thread runs `run_daemon()` in a loop until the actor is deactivated.

```cpp
class DaemonActor : public EventBasedActor {
public:
    DispatchPolicy dispatch_policy() const override { return DispatchPolicy::DedicatedThread; }

    // Override to provide the daemon's main loop body.
    // Called repeatedly from the dedicated thread.
    // Return false to exit the loop (actor is shutting down).
    virtual bool run_once() = 0;

    // Optional: called when the dedicated thread starts, before run_once loop.
    virtual void on_daemon_start() {}

    // Optional: called when the dedicated thread stops, after run_once loop.
    virtual void on_daemon_stop() {}

    // Access the underlying thread handle (for affinity, priority, etc.)
    std::thread& daemon_thread() { return daemon_thread_; }

protected:
    DaemonActor(ActorContext* ctx, ActorSystem& sys);

    // Override dispatch_hints for CPU affinity
    DispatchHints dispatch_hints() const override {
        return hints_;
    }
    void set_cpu_affinity(int core) { hints_.cpu_affinity = core; }

private:
    void on_activate() override;
    void on_deactivate() override;

    void daemon_loop();  // runs on dedicated thread: on_daemon_start() → run_once() loop → on_daemon_stop()

    std::thread daemon_thread_;
    std::atomic<bool> running_{false};
    DispatchHints hints_;
};
```

**Usage pattern:**
```cpp
class MyDbProxy : public DaemonActor {
    bool run_once() override {
        // Block on epoll / io_uring / DPDK rx queue
        auto events = epoll_wait(epoll_fd_, ...);
        for (auto& ev : events) {
            // Translate to actor messages and send to self or others
            context()->send(self_addr_, parse_event(ev));
        }
        return true;  // keep running
    }

    Behavior make_behavior() override {
        return {
            [this](const DbQuery& q) { /* enqueue to async I/O */ },
            [this](const IoComplete& r) { /* reply to original sender */ },
        };
    }
};
```

### 6.3 PollingActor

A `DaemonActor` specialized for periodic resource polling — the canonical use case is DPDK packet processing where `run_once()` busy-polls a NIC rx queue and never blocks.

```cpp
class PollingActor : public DaemonActor {
public:
    // Polling actors typically want CPU affinity (dedicated core)
    PollingActor(ActorContext* ctx, ActorSystem& sys, int cpu_core = -1);

    // Set polling budget: max packets/events per run_once() call
    void set_poll_budget(uint32_t max_events) { poll_budget_ = max_events; }
    uint32_t poll_budget() const { return poll_budget_; }

protected:
    uint32_t poll_budget_ = 64;
};
```

**DPDK example:**
```cpp
class DpdkRxActor : public PollingActor {
public:
    DpdkRxActor(ActorContext* ctx, ActorSystem& sys, uint16_t port_id)
        : PollingActor(ctx, sys, /*cpu_core=*/2), port_id_(port_id) {}

    bool run_once() override {
        struct rte_mbuf* bufs[poll_budget()];
        uint16_t nb_rx = rte_eth_rx_burst(port_id_, 0, bufs, poll_budget());
        for (uint16_t i = 0; i < nb_rx; i++) {
            // Send packet to pipeline actors via standard messaging
            context()->send(pipeline_addr_, PacketMsg{bufs[i]});
        }
        return true;  // never stop
    }

    Behavior make_behavior() override {
        return {
            [this](const PortConfig& cfg) { /* reconfigure port */ },
        };
    }

private:
    uint16_t port_id_;
};
```

### 6.4 DenseComputingActor

An `EventBasedActor` that runs its handlers on a dedicated thread pool instead of the cooperative M:N workers. This prevents long-running computation from starving message-processing actors.

```cpp
class DenseComputingActor : public EventBasedActor {
public:
    DenseComputingActor(ActorContext* ctx, ActorSystem& sys,
                        uint32_t pool_size = 1);

    DispatchPolicy dispatch_policy() const override { return DispatchPolicy::DedicatedPool; }

    DispatchHints dispatch_hints() const override {
        DispatchHints h;
        h.pool_size = pool_size_;
        return h;
    }

    uint32_t pool_size() const { return pool_size_; }

protected:
    uint32_t pool_size_;
};
```

**Usage:**
```cpp
class MlInferenceActor : public DenseComputingActor {
public:
    MlInferenceActor(ActorContext* ctx, ActorSystem& sys)
        : DenseComputingActor(ctx, sys, /*pool_size=*/4) {}

    Behavior make_behavior() override {
        return {
            [this](const InferenceRequest& req) {
                // This runs on the DedicatedPool — never blocks cooperative workers
                auto result = model_.run(req.input());
                context()->reply(InferenceResult{result});
            },
        };
    }

private:
    Model model_;
};
```

### 6.5 ExternalMsgGatewayActor

A `DaemonActor` that acts as an ingress gateway, translating external protocol messages (HTTP, gRPC, WebSocket) into internal actor messages.

```cpp
class ExternalMsgGatewayActor : public DaemonActor {
public:
    ExternalMsgGatewayActor(ActorContext* ctx, ActorSystem& sys);

    // Map an external request pattern to an internal actor
    void route(const std::string& path_pattern, ActorAddr target);
    void route(const std::string& path_pattern, ActorRef target);

    // Transform external payload to internal message
    using PayloadTransform = std::function<MessageVariant(StreamBuffer)>;
    void set_transform(const std::string& content_type, PayloadTransform tx);

protected:
    // Subclasses implement protocol-specific accept/parse in run_once()
    std::unordered_map<std::string, ActorAddr> routes_;
    std::vector<PayloadTransform> transforms_;
};
```

### 6.6 HTTPServerActor

Concrete HTTP server built on `ExternalMsgGatewayActor`.

```cpp
class HTTPServerActor : public ExternalMsgGatewayActor {
public:
    HTTPServerActor(ActorContext* ctx, ActorSystem& sys,
                    const std::string& bind_addr, uint16_t port);

    bool run_once() override;

    // Register HTTP method + path → actor handlers
    void get(const std::string& path, ActorAddr handler);
    void post(const std::string& path, ActorAddr handler);
    void put(const std::string& path, ActorAddr handler);
    void del(const std::string& path, ActorAddr handler);

private:
    struct RouteEntry {
        std::string method;
        std::string path;
        ActorAddr handler;
    };
    std::vector<RouteEntry> http_routes_;

    // Owns an epoll/kqueue/io_uring event loop for the listening socket
    int listen_fd_ = -1;
    std::string bind_addr_;
    uint16_t port_;
};
```

---

## 7. Scheduler Integration

### 7.1 IScheduler Changes

The `IScheduler` interface is extended to support dispatching actors to dedicated threads/pools.

```cpp
class IScheduler {
public:
    virtual ~IScheduler() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    // Cooperative scheduling (existing)
    virtual void notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;
    virtual void notify_idle(ActorId actor) = 0;
    virtual void yield(ActorId actor, uint8_t priority) = 0;

    // Pinned dispatch — register an actor that needs a dedicated thread/pool
    // Called by ActorHost when activating an actor with non-Cooperative policy.
    virtual void register_dedicated_thread(ActorId actor, int cpu_affinity) = 0;
    virtual void register_dedicated_pool(ActorId actor, uint32_t pool_size) = 0;

    // Shutdown a dedicated execution context for an actor
    virtual void unregister_dedicated(ActorId actor) = 0;

    // Timer management (existing)
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;
    virtual TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) = 0;
    virtual void cancel_timer(TimerHandle handle) = 0;

    virtual size_t worker_count() const = 0;
    virtual bool is_running() const = 0;
};
```

### 7.2 DedicatedThreadPool (for DedicatedPool policy)

A fixed-size thread pool owned by the scheduler, used exclusively by `DenseComputingActor` instances with `DedicatedPool` policy.

```cpp
class DedicatedThreadPool {
public:
    explicit DedicatedThreadPool(uint32_t num_threads);
    ~DedicatedThreadPool();

    void start();
    void stop();

    // Enqueue work for an actor on this pool
    void enqueue(ActorId actor, std::function<void()> work);

    size_t pending() const;

private:
    void worker_loop(uint32_t worker_id);

    uint32_t num_threads_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};

    // One MPSC queue per worker, with work-stealing between them
    std::vector<std::unique_ptr<ChaselevDeque<WorkItem>>> queues_;
};
```

### 7.3 Dispatch Flow

```
ActorSystem::spawn(actor)
    │
    ▼
ActorHost::activate_actor(actor)
    │
    ├── policy == Cooperative ──▶ register with work-stealing pool (existing path)
    │
    ├── policy == DedicatedThread ──▶ scheduler.register_dedicated_thread(actor, hints.cpu_affinity)
    │       │                            └── creates std::thread, runs daemon_loop()
    │       │
    │       └── Messages to this actor:
    │            1. Enqueued to mailbox (normal path)
    │            2. Scheduler wakes the dedicated thread if it's blocked on I/O
    │            3. Dedicated thread drains mailbox, then resumes I/O/daemon loop
    │
    └── policy == DedicatedPool ──▶ scheduler.register_dedicated_pool(actor, hints.pool_size)
            │                            └── creates DedicatedThreadPool, assigns actor to it
            │
            └── Messages to this actor:
                 1. Enqueued to mailbox (normal path)
                 2. notify_ready() enqueues to DedicatedPool instead of global pool
                 3. DedicatedPool worker dequeues and calls actor->receive()
```

---

## 8. ActorContext and ActorSystem

### 8.1 ActorContext

Execution context for actors. Enables spawning children, sending messages, and scheduling.

```cpp
class ActorContext {
public:
    ActorContext(Actor owner);
    ~ActorContext();

    // Spawn child actors (C++20: template, not auto params)
    template<typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);

    template<typename T, typename... Args>
    T spawn(Args&&... args);

    // Send messages
    void send(const ActorAddress& target, MessageVariant msg);

    // Replies
    void reply(MessageVariant msg);
    void reply_with_error(error err);

    // Scheduled execution
    void schedule(std::chrono::milliseconds delay, MessageVariant msg);

    // Children management
    std::vector<Actor> children() const;
    void add_child(Actor child);
    void remove_child(Actor child);

    // Link management
    std::vector<ActorAddress> linked_actors() const;

    // Monitoring
    void monitor(const ActorAddress& target);

private:
    Actor owner_;
    std::vector<Actor> children_;
    std::vector<ActorAddress> linked_;
    std::vector<ActorAddress> monitored_;
};
```

### 8.2 ActorSystem

The actor environment containing schedulers, registry, and configuration.

```cpp
class ActorSystem {
public:
    explicit ActorSystem(const Config& config);
    ~ActorSystem();

    // Spawn actors at system level (C++20: template, not auto params)
    template<typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);

    template<typename T, typename... Args>
    T spawn(Args&&... args);

    // Actor registry (for named lookups)
    void register_actor(const std::string& name, Actor actor);
    Actor resolve_actor(const std::string& name);
    void unregister_actor(const std::string& name);

    // Actor type registration
    void register_actor_type(const ActorTypeDef& def);
    ActorTypeDef get_actor_type(ActorType type) const;

    // Clock for time-based operations
    Clock& clock() { return clock_; }

    // System actor (spawned first)
    Actor system_actor() { return system_actor_; }

    // Scheduler access
    sched::IScheduler& scheduler() { return *scheduler_; }

    // Registry access
    actor_registry& registry() { return registry_; }

private:
    Config config_;
    Clock clock_;
    actor_registry registry_;
    std::unique_ptr<sched::IScheduler> scheduler_;
    std::unordered_map<ActorType, ActorTypeDef> actor_types_;
    Actor system_actor_;
};
```

---

## 9. Behavior and Message Handling

### 9.1 Behavior

A behavior is a set of message handlers that defines an actor's response to messages.

```cpp
class Behavior {
public:
    Behavior() = default;

    template<typename... Handlers>
    Behavior(Handlers&&... handlers);

    // Match and invoke handler for message
    result<MessageVariant> invoke(MessageVariant& msg);

    // Check if any handler matches
    bool matches(const MessageVariant& msg) const;

    // Chain behaviors
    Behavior or_else(const Behavior& other) const;

    // Add handler
    template<typename T>
    void add(T&& type_tag, message_handler handler);

private:
    std::vector<message_handler> handlers_;
};

// message_handler - wraps a handler function
class message_handler {
public:
    template<typename T, typename F>
    message_handler(T&& type_tag, F&& func);

    result<MessageVariant> operator()(MessageVariant& msg);
    bool matches(const MessageVariant& msg) const;

private:
    std::function<result<MessageVariant>(MessageVariant&)> func_;
    std::type_index type_;
};
```

### 9.2 Typed Behavior

```cpp
template<typename... Signatures>
class typed_behavior {
public:
    using result_type = typed_result<Signatures...>;

    template<typename T>
    typed_behavior& operator()(T&& handler);

    result<MessageVariant> invoke(MessageVariant& msg);
    bool matches(const MessageVariant& msg) const;

private:
    std::tuple<message_handler<Signatures>...> handlers_;
};

template<typename T>
struct handler_type;

template<typename R, typename... Args>
struct handler_type<result<R>(Args...)> {
    using result = R;
    using args = std::tuple<Args...>;

    template<typename F>
    result operator()(F&& f, Args... args) {
        return f(args...);
    }
};
```

### 9.3 Result Type

```cpp
template<typename T>
class result {
public:
    static result<T> make(T&& value);
    static result<T> make(error err);

    bool has_value() const { return has_value_; }
    T& value() { return std::get<0>(value_); }
    const error& error() const { return std::get<1>(value_); }

private:
    bool has_value_;
    std::variant<T, error> value_;
};

template<>
class result<void> {
public:
    static result<void> make();
    static result<void> make(error err);

    bool has_value() const { return has_value_; }
    void value() const {}
    const error& error() const { return error_; }

private:
    bool has_value_;
    error error_;
};
```

---

## 10. Supervision

### 10.1 Supervision Types

```cpp
enum class SupervisionDirective { Restart, Stop, Escalate };

struct ChildFailure {
    ActorId child_id;
    error reason;
    SupervisionDirective directive;
};

struct SupervisionPolicy {
    enum class Strategy { OneForOne, AllForOne, OneForAll };
    Strategy strategy = Strategy::OneForOne;
    uint32_t max_restarts = 10;
    std::chrono::milliseconds restart_interval{5000};
};
```

### 10.2 Supervisor Interface

```cpp
class Supervisor {
public:
    virtual ~Supervisor() = default;
    virtual SupervisionDirective on_child_failure(const ChildFailure& failure) = 0;
    virtual void on_child_stopped(ActorId child_id);
};

class OneForOneSupervisor : public Supervisor {
public:
    explicit OneForOneSupervisor(SupervisionPolicy policy = {});
    SupervisionDirective on_child_failure(const ChildFailure& failure) override;
private:
    SupervisionPolicy policy_;
};

class AllForOneSupervisor : public Supervisor {
public:
    explicit AllForOneSupervisor(SupervisionPolicy policy = {});
    SupervisionDirective on_child_failure(const ChildFailure& failure) override;
private:
    SupervisionPolicy policy_;
};
```

### 10.3 SupervisorActor

```cpp
class SupervisorActor : public EventBasedActor {
public:
    SupervisorActor(ActorContext* ctx, ActorSystem& sys,
                    Supervisor& strategy, std::vector<Actor> children);

protected:
    Behavior make_behavior() override;

    // Override to add spawn logic for restarted children.
    // The base implementation manages restart counts and the sliding window.
    virtual void restart_child(ActorId child_id);
    void restart_all_children();

    Supervisor& strategy_;
    std::vector<Actor> children_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
    std::chrono::steady_clock::time_point first_failure_time_;

private:
    void handle_child_down(TypeTag tag, const StreamBuffer& payload);
};
```

### 10.4 SelfSupervisingActor

```cpp
class SelfSupervisingActor : public EventBasedActor {
public:
    SelfSupervisingActor(ActorContext* ctx, ActorSystem& sys,
                         SupervisionPolicy policy = SupervisionPolicy{});

    void add_child(Actor child);
    void remove_child(Actor child);

    // Remote child management
    void add_remote_child(ActorRef child);
    bool has_remote_child(const ActorAddress& addr) const;
    ActorRef get_remote_child(const ActorAddress& addr) const;
    void remove_remote_child(const ActorAddress& addr);
    const std::vector<ActorRef>& remote_children() const { return remote_children_; }

protected:
    virtual SupervisionDirective on_failure(ActorId child_id, const error& err);

private:
    SupervisionDirective decide_restart(ActorId child_id, const error& err);
    std::vector<Actor> children_;
    SupervisionPolicy policy_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
    std::chrono::steady_clock::time_point first_failure_time_;
    std::vector<ActorRef> remote_children_;
};
```

### 10.5 Supervision of Pinned Actors

When a `DaemonActor` (or any actor with `DedicatedThread`/`DedicatedPool` policy) fails:

1. The dedicated thread or pool worker catches the failure (via `on_exit()` or unhandled exception equivalent).
2. A `down_msg` is sent to the supervisor, exactly as for a cooperative actor.
3. The supervisor's `handle_child_down()` applies the configured `SupervisionPolicy`.
4. On `Restart`, `ActorHost` spawns a new instance of the actor, which gets a fresh dedicated thread/pool.
5. The supervisor **does not know** the child was pinned — the dispatch policy is opaque to supervision.

---

## 11. Actor Lifecycle and Passivation

### 11.1 Lifecycle States

The actor lifecycle state machine is implemented in `lifecycle_state.hpp` and
`lifecycle_actor.hpp`. All transitions are validated against a constexpr
transition table and executed via CAS.

```cpp
enum class LifecycleState : uint8_t {
    kStarting    = 0,  // Initial spawn, not yet accepting user messages
    kActive      = 1,  // Processing user messages normally
    kDraining    = 2,  // Draining mailbox before stop/passivation
    kStopping    = 3,  // Final cleanup before termination
    kStopped     = 4,  // Terminated, no messages accepted
    kFailed      = 5,  // Failure, awaiting supervisor decision
    kRecovering  = 6,  // Restoring durable state before re-entering kActive
    kQuarantined = 7,  // Isolated — rejects user messages, accepts system messages
    kPassivating = 8,  // Draining + snapshotting before hibernation
    kPassivated  = 9,  // Memory freed, route stub alive, durable state stored
};
```

Each state carries metadata in a constexpr `StateDef` table:

```cpp
struct StateDef {
    LifecycleState state;
    const char* name;
    bool accepts_user_msgs : 1;
    bool accepts_system_msgs : 1;
    uint8_t num_transitions : 3;
    LifecycleState transitions[7];
};
```

Key properties:
- **`kActive`** is the only state that accepts user messages. All others gate
  them.
- **`kRecovering`** is entered from `kFailed` (durable recovery after crash) or
  from `kPassivated` (reactivation triggered by an incoming message).
- **`kPassivating`** drains the mailbox and persists a snapshot before releasing
  memory. It accepts system messages (link/unlink, shutdown, CLI inspect) but
  rejects user messages.
- **`kPassivated`** has no live actor object — a lightweight `LocalPassivatedRoute`
  stub owns the lifecycle state in the registry.

### 11.2 LifecycleActor

`LifecycleActor` is a mixin providing CAS-based state transitions with virtual
hooks invoked after each transition:

```cpp
class LifecycleActor {
public:
    LifecycleState state() const noexcept;
    bool accepts_user_msgs() const noexcept;
    bool accepts_system_msgs() const noexcept;
    uint64_t incarnation() const noexcept;

    bool transition(LifecycleState to);  // validates + CAS + invokes hook

    // Virtual hooks (default = no-op)
    virtual void on_start() {}
    virtual void on_drain() {}
    virtual void on_drain_timeout() {}
    virtual void on_stop() {}
    virtual void on_deactivate() {}
    virtual void on_fail(error err);
    virtual void on_recover() {}
    virtual void on_restart() {}
    virtual void on_quarantined(QuarantineReason reason) {}
    virtual void on_passivating() {}
    virtual void on_passivated() {}

protected:
    std::atomic<uint8_t> state_;
    std::atomic<uint64_t> incarnation_;
    error failure_reason_{0};
    DrainConfig drain_config_{};
    QuarantineReason quarantine_reason_{};
    std::chrono::steady_clock::time_point quarantined_at_{};
};
```

### 11.3 Passivation Protocol

Passivation releases an idle actor's memory while preserving its identity and
(optionally) its durable state. The actor can later be reactivated when a
message arrives.

**Triggers:**

| Trigger | Mechanism | Config |
|---------|-----------|--------|
| Idle timeout | `TimingWheel` timer reset on each user message processed | `PassivationConfig::idle_timeout` (0 = disabled) |
| Self-request | `ActorContext::passivate()` after work completes | N/A |
| Memory pressure | `MemoryPressureMonitor` selects LRU actors above high watermark | `PassivationConfig::allow_memory_pressure` (default true) |
| CLI/admin | `/actor passivate <id>` | CLI enabled |

**Passivation flow:**
1. `Active → Passivating`: drain mailbox using existing `DrainConfig`
2. If durable: persist snapshot via `DurableStateStore::write_snapshot()`
3. If memory-only: serialize via `Hibernatable` → `HibernationRegistry::store()`
4. `Passivating → Passivated`: release actor memory, install `LocalPassivatedRoute` stub

**Reactivation flow:**
1. Message arrives at `LocalPassivatedRoute::try_deliver()`
2. Buffered in bounded reactivation queue; `reactivation_in_progress` flag set
3. `Passivated → Recovering`
4. Restore state from `DurableStateStore` or `HibernationRegistry`
5. `Recovering → Active`: new actor instance replaces route stub; buffered messages delivered

### 11.4 Durable Actor State

#### IDurableActor

Opt-in interface for actors that survive passivation across process restarts:

```cpp
class IDurableActor {
public:
    virtual ~IDurableActor() = default;
    virtual std::string_view persistence_id() const = 0;
    virtual result<StreamBuffer> snapshot_state() const = 0;
    virtual result<void> restore_snapshot(const StreamBuffer& data) = 0;
    virtual result<void> apply_event(const StreamBuffer& event) { return success(); }
    virtual result<StreamBuffer> migrate_snapshot(uint32_t from_version,
                                                   const StreamBuffer& data) {
        return error::make(FailureReason::SchemaVersionMismatch);
    }
};
```

#### DurableStateStore

Abstracts the persistence backend:

```cpp
class DurableStateStore {
public:
    virtual ~DurableStateStore() = default;
    virtual result<SnapshotRecord> write_snapshot(std::string_view persistence_id,
                                                   uint32_t schema_version,
                                                   StreamBuffer data) = 0;
    virtual result<SnapshotRecord> load_latest_snapshot(
        std::string_view persistence_id) = 0;
    virtual result<void> append_event(std::string_view persistence_id,
                                      uint64_t sequence, StreamBuffer event) = 0;
    virtual result<std::vector<EventRecord>> load_events_after(
        std::string_view persistence_id, uint64_t after_sequence) = 0;
    virtual result<void> delete_state(std::string_view persistence_id) = 0;
};
```

Initial implementations: `InMemoryStateStore` (tests), `FileStateStore`
(local durability with atomic rename + CRC32C).

### 11.5 Route Handling

The `IActorRoute` interface decouples message delivery from whether the actor
is active, passivated, or remote:

```cpp
class IActorRoute {
public:
    virtual ~IActorRoute() = default;
    virtual EnqueueResult try_deliver(TypedMessage msg) = 0;
    virtual bool is_active() const = 0;
    virtual LifecycleState state() const = 0;
    virtual std::string describe() const = 0;
};
```

Three implementations:

| Route | When | Behavior |
|-------|------|----------|
| `LocalActiveRoute` | Actor is live | Delegates directly to actor mailbox |
| `LocalPassivatedRoute` | Actor is passivated | Buffers message, triggers reactivation |
| `ShardOwnedRoute` | Future: cross-node sharding | Forwards to shard owner |

`LocalPassivatedRoute` holds the actor's `ActorId`, `persistence_id`,
`PassivationRecord`, a bounded `MpscRingBuffer` reactivation queue (default
capacity 64), and an atomic `reactivation_in_progress` flag. When a message
arrives, it sets the flag, buffers the message, and spawns a reactivation task.
Subsequent messages see the flag already set and simply enqueue. If the buffer
fills, `try_deliver()` returns `EnqueueResult::Rejected` with
`FailureReason::PassivationQueueFull`.

### 11.6 Passivation Configuration

```cpp
struct PassivationConfig {
    std::chrono::milliseconds idle_timeout{0};  // 0 = disabled
    bool durable = false;
    bool allow_memory_pressure = true;
    uint32_t schema_version = 1;
};
```

System-level defaults in TOML `[system.passivation]` with per-actor overrides
in `[[actor]]` blocks. Compile-time gate: `ENABLE_ACTOR_PASSIVATION` (default
ON). When disabled, all passivation paths are eliminated via
`HPACTOR_ENABLE_PASSIVATION` preprocessor guard — zero overhead for
non-passivated actors.

### 11.7 Memory-Level Hibernation (Existing)

`Hibernatable` provides memory-only serialization without durable storage:

```cpp
class Hibernatable {
public:
    virtual ~Hibernatable() = default;
    virtual size_t serialized_size() const = 0;
    virtual void serialize_to(std::span<std::byte> buffer) const = 0;
    virtual void deserialize_from(std::span<const std::byte> buffer) = 0;
};
```

`HibernationRegistry` is a singleton, mutex-protected map of
`ActorId → HibernationBuffer`. Used by actors that want memory release without
durable persistence. An actor may implement both `IDurableActor` and
`Hibernatable`: `IDurableActor` takes precedence for passivation;
`Hibernatable` remains available for non-passivation cold-storage use.
```

---

## 12. Actor References and Addresses

### 12.1 ActorAddress

```cpp
struct ActorAddress {
    NodeId node_id = 0;
    ActorType type = 0;
    ActorId id;
    uint64_t incarnation = 0;

    bool operator==(const ActorAddress& other) const;
    bool is_local() const { return node_id == 0 || node_id == local_node_id(); }
    explicit operator bool() const { return id.value() != 0; }

    bytes serialize() const;
    static ActorAddress deserialize(bytes_view data);

private:
    static NodeId local_node_id();
};

using ActorAddr = ActorAddress;
constexpr ActorAddr invalid_actor_addr{};
```

### 12.2 Actor Reference

```cpp
class Actor {
public:
    Actor() = default;
    explicit Actor(std::shared_ptr<abstract_actor> ptr);

    ActorId id() const;
    ActorType type() const;
    ActorAddress address() const;

    operator ActorAddress() const;
    operator ActorAddr() const;
    explicit operator bool() const;

    void swap(Actor& other) noexcept;

private:
    std::shared_ptr<abstract_actor> actor_;
};
```

### 12.3 Typed Actor Reference

```cpp
template<typename... Signatures>
class typed_actor {
public:
    using base_type = typed_event_based_actor<Signatures...>;

    typed_actor() = default;
    explicit typed_actor(std::shared_ptr<base_type> ptr);

    template<typename T>
    void operator()(T&& msg);

    ActorId id() const;
    ActorAddress address() const;
    operator ActorAddress() const;
    explicit operator bool() const;

private:
    std::shared_ptr<base_type> actor_;
};
```

### 12.4 ActorProxy

```cpp
class ActorProxy {
public:
    ActorProxy(ActorAddress address, Transport* transport);

    ActorAddress address() const { return address_; }
    bool is_local() const { return address_.is_local(); }

    template<typename T>
    Task<T> invoke(T&& msg, std::chrono::milliseconds timeout = {});

    template<typename T>
    void send(T&& msg);

private:
    ActorAddress address_;
    Transport* transport_;
};
```

### 12.5 Unified Reference

```cpp
class ActorRef {
public:
    ActorRef() = default;
    ActorRef(Actor actor);
    ActorRef(ActorProxy proxy);

    ActorAddress address() const;
    bool is_local() const;

    template<typename T>
    void send(T&& msg);

    template<typename T>
    Task<T> invoke(T&& msg, std::chrono::milliseconds timeout = {});

private:
    std::variant<Actor, ActorProxy> ref_;
};
```

---

## 13. Mailbox Integration

### 13.1 Message Types

```cpp
struct ActorMessage {
    MessageId id;
    ActorAddress sender;
    ActorAddress receiver;
    std::string method_name;
    MessageVariant payload;
    bool is_one_way = false;
    TraceContext trace;
};

// Common system messages
struct down_msg {
    ActorAddress terminated_actor;
    error reason;
};

struct exit_msg {
    ActorAddress sender;
    error reason;
};

struct link_msg {
    ActorAddress target;
};

struct unlink_msg {
    ActorAddress target;
};

using MessageVariant = std::variant<
    down_msg, exit_msg, link_msg, unlink_msg
    // ... user-defined types
>;
```

---

## 14. File Structure (Updated)

```
include/hpactor/
├── platform.hpp
├── message.hpp
├── mailbox.hpp
├── mutex_mailbox.hpp
│
├── actor/
│   ├── actor_fwd.hpp              # Forward declarations
│   ├── abstract_actor.hpp         # abstract_actor + DispatchPolicy
│   ├── local_actor.hpp            # local_actor
│   ├── event_based_actor.hpp      # event_based_actor
│   ├── typed_actor.hpp            # typed_actor<>, typed_event_based_actor<>
│   ├── stateful_actor.hpp         # stateful_actor<T>
│   ├── blocking_actor.hpp         # blocking_actor
│   ├── scoped_actor.hpp           # scoped_actor
│   │
│   ├── daemon_actor.hpp           # DaemonActor (DedicatedThread)
│   ├── polling_actor.hpp          # PollingActor (DPDK-style)
│   ├── dense_computing_actor.hpp  # DenseComputingActor (DedicatedPool)
│   ├── external_msg_gateway.hpp   # ExternalMsgGatewayActor
│   ├── http_server_actor.hpp      # HTTPServerActor
│   │
│   ├── lifecycle_state.hpp        # LifecycleState enum + constexpr StateDef table
│   ├── lifecycle_actor.hpp        # LifecycleActor mixin
│   ├── passivation_config.hpp     # PassivationConfig, PassivationRecord (NEW)
│   ├── durable_actor.hpp          # IDurableActor interface (NEW)
│   ├── durable_state_store.hpp    # DurableStateStore, SnapshotRecord, EventRecord (NEW)
│   ├── actor_route.hpp            # IActorRoute, LocalActiveRoute, LocalPassivatedRoute (NEW)
│   └── memory_pressure_monitor.hpp # MemoryPressureMonitor (NEW)
│
├── actor_context.hpp              # ActorContext (+ passivate())
├── actor_system.hpp               # ActorSystem (+ DurableStateStore, PassivationManager)
├── behavior.hpp                   # Behavior, message_handler
├── typed_behavior.hpp             # typed_behavior<>
├── result.hpp                     # result<T>
│
├── mem/
│   ├── hibernatable.hpp           # Hibernatable interface
│   └── hibernation_registry.hpp   # HibernationRegistry singleton
│
├── sched/
│   ├── scheduler.hpp              # IScheduler + HybridScheduler
│   ├── worker_thread.hpp          # WorkerThread
│   ├── dedicated_thread_pool.hpp  # DedicatedThreadPool
│   └── dispatch_policy.hpp        # DispatchPolicy, DispatchHints
│
├── supervision/
│   ├── supervision.hpp
│   ├── one_for_one_supervisor.hpp
│   └── all_for_one_supervisor.hpp
│
├── fault/
│   ├── fault_controller.hpp
│   └── passivation_fault_points.hpp  # (NEW) 12 fault injection points
│
└── ref/
    ├── actor_address.hpp
    ├── actor_ref.hpp
    ├── typed_actor_ref.hpp
    └── actor_proxy.hpp
```

---

## 15. Implementation Phases

### Phase A: Core Actor (Completed)
- `abstract_actor`, `local_actor`, `event_based_actor`
- `Behavior`, `message_handler`
- Basic `ActorSystem` and `ActorContext`
- Unit tests

### Phase B: Typed Actors (Completed)
- `typed_event_based_actor<Signatures...>`
- `typed_behavior<Signatures...>`
- `result<T>`
- Typed actor spawn factory

### Phase C: Supervision (Completed)
- `Supervisor`, `OneForOneSupervisor`, `AllForOneSupervisor`
- `supervisor_actor`
- `down_msg`, `exit_msg` handling
- Link/unlink/monitor

### Phase D: Stateful & Blocking Actors (Completed)
- `stateful_actor<T>`
- `blocking_actor`, `scoped_actor`
- `receive()` implementations

### Phase E: DispatchPolicy + Pinned Dispatcher (Completed)
- `DispatchPolicy` enum and `DispatchHints` struct
- `IScheduler` extensions: `register_dedicated_thread()`, `register_dedicated_pool()`
- `DedicatedThreadPool` implementation
- `DaemonActor` base class with dedicated thread lifecycle
- Unit tests

### Phase F: Special Actors (Completed)
- `PollingActor` (DPDK-style busy-poll)
- `DenseComputingActor` (DedicatedPool)
- `ExternalMsgGatewayActor` (protocol ingress)
- `HTTPServerActor` (HTTP server on top of gateway)
- Unit tests + integration tests

### Phase G: Lifecycle & Hibernation (Completed — Different from original design)
- `LifecycleState` enum (10 states including `kPassivating`, `kPassivated`)
- `LifecycleActor` mixin with CAS-based transitions
- `Hibernatable` interface + `HibernationRegistry` (memory-level only)
- `PassivationConfig`, `IDurableActor`, `DurableStateStore` — see [ACT-008 Design Spec](../../superpowers/specs/2026-06-06-act-008-actor-passivation-design.md)

### Phase H: References & Proxy (Completed)
- `ActorAddress`, `ActorRef`
- `ActorProxy` for remote actors
- `IActorRoute` interface — `LocalActiveRoute`, `LocalPassivatedRoute`
- Type-safe handles

---

## 16. Open Questions

- [ ] What is the default idle timeout before passivation? (Current proposal: 10 min)
- [ ] Should blocking actors be spawned with explicit thread affinity?
- [ ] Maximum number of children per supervisor?
- [ ] What error types should be defined by default?
- [ ] Should `DedicatedPool` actors share pools when `pool_size` and `cpu_affinity` match, or always get their own?
- [ ] For `DaemonActor`: should the dedicated thread drain the mailbox inline, or should mailbox messages be dispatched to the cooperative pool and only the daemon loop runs on the dedicated thread?
- [ ] What is the default reactivation queue capacity? (Current proposal: 64)
- [ ] Should passivation support a "grace period" where recently-active actors cannot be passivated by memory pressure?
- [ ] Should `LocalPassivatedRoute` support priority-aware reactivation (higher-priority messages trigger reactivation, lower-priority messages just buffer)?

---

## 17. References

- [CAF Actor Types](actor/Actors.rst)
- [Distributed Actor System Architecture](../Distributed%20Actor%20System%20Architecture.md)
- [Previous Actor Framework Design](../2026-04-10-cpp-actor-framework-design.md)
