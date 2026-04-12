# C++ Actor Framework - Actor Core Concept and Design Specification

> **Spec Status:** Draft
> **Created:** 2026-04-11
> **Based on:** Distributed Actor System Architecture.md

## 1. Executive Summary

This document specifies the Actor implementation for the HPActor C++20 framework. Actors follow an **event-based** programming model with **turn-based concurrency**, supporting both **statically typed** and **dynamically typed** messaging. The design is inspired by the CAF (C++ Actor Framework) and aligns with the distributed system architecture.

**Key Design Decisions:**
- Event-based actors with cooperative scheduling
- Explicit lifecycle with optional hibernation
- Typed and dynamically typed actor support
- Hierarchical supervision (OneForOne, AllForOne)
- Actor references: local handles + distributed addresses

---

## 2. Fundamental Types

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

## 3. Actor Type Hierarchy

```
┌─────────────────────────────────────────────────────────────┐
│                      abstract_actor                         │
│  - ActorId id()                                            │
│  - ActorAddress address()                                   │
│  - ActorSystem& system()                                    │
│  - link_to(), unlink_from()                                │
│  - monitor(), demonitor()                                  │
│  - spawn() from context                                    │
└──────────────────────────┬──────────────────────────────────┘
                           │
           ┌───────────────┴───────────────┐
           ▼                               ▼
┌─────────────────────┐       ┌─────────────────────────────┐
│    local_actor      │       │         remote_actor          │
│  - home_system()    │       │  (placeholder for distrib)   │
│  - ctx->spawn()      │       │                              │
└─────────┬───────────┘       └──────────────────────────────┘
          │
    ┌─────┴──────────────────────────────────────┐
    ▼              ▼              ▼              ▼
┌─────────┐ ┌──────────┐ ┌────────────┐ ┌───────────────┐
│ Event   │ │ Blocking │ │  Typed    │ │  Dynamically  │
│ Based   │ │  Actor   │ │  Actor<>  │ │    Typed      │
│ Actor   │ │          │ │ (static)  │ │   Actor       │
└────┬────┘ └────┬─────┘ └─────┬──────┘ └───────┬───────┘
     │           │             │                │
     ▼           ▼             ▼                ▼
┌──────────┐ ┌──────────┐ ┌──────────────────────────┐
│Stateful  │ │ Scoped   │ │    typed_event_based_actor<> │
│Actor<T>  │ │ Actor    │ │    (CAF-style)            │
└──────────┘ └──────────┘ └───────────────────────────┘
```

### 2.1 abstract_actor

Base class for all actors. Provides identity, linking, and monitoring.

```cpp
class abstract_actor : public std::enable_shared_from_this<abstract_actor> {
public:
    virtual ~abstract_actor() = default;
    
    ActorId id() const { return id_; }
    ActorAddress address() const;
    ActorSystem& system() { return system_; }
    
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
    
private:
    ActorId id_;
    ActorSystem& system_;
};
```

### 2.2 local_actor

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

### 2.3 event_based_actor

Cooperatively scheduled actor with behavior-based message handling.

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

### 2.4 typed_event_based_actor

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

Example usage:
```cpp
using calculator_actor = typed_actor<
    result<int>(add, int, int),
    result<int>(subtract, int, int),
    result<void>(shutdown)
>;

class calculator : public calculator_actor::base_type {
protected:
    behavior_type make_behavior() override;
};
```

### 2.5 stateful_actor

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

Usage:
```cpp
struct counter_state {
    int value = 0;
};

class counter : public stateful_actor<counter_state> {
protected:
    Behavior make_behavior() override {
        return {
            [this](increment) { state().value++; },
            [this](get_value) -> int { return state().value; }
        };
    }
};
```

### 2.6 blocking_actor

Actor running in its own thread with blocking receive.

```cpp
class blocking_actor : public local_actor {
public:
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

### 2.7 scoped_actor

Blocking actor for non-actor contexts.

```cpp
class scoped_actor : public blocking_actor {
public:
    explicit scoped_actor(ActorSystem& sys);
    ~scoped_actor();
    
    template<typename T>
    T receive();
};
```

### 2.8 Additional Actor Types

```cpp
// virtual_actor - Orleans-style, activated on-demand (distributed)
class virtual_actor : public abstract_actor {
public:
    virtual Task<> on_activation();
    virtual Task<> on_passivation();
    virtual Task<> save_state() = 0;
    virtual Task<> load_state() = 0;
};

// broker - for middleware/proxy scenarios
class broker : public event_based_actor {
public:
    void relay(MessageVariant&& msg);
    std::vector<Actor> peers() const;
};
```

---

## 3. ActorContext and ActorSystem

### 3.1 ActorContext

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

### 3.2 ActorSystem

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
    
    // Registry access
    actor_registry& registry() { return registry_; }
    
private:
    Config config_;
    Clock clock_;
    actor_registry registry_;
    std::unordered_map<ActorType, ActorTypeDef> actor_types_;
    Actor system_actor_;
};
```

---

## 4. Behavior and Message Handling

### 4.1 Behavior

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

### 4.2 Typed Behavior

```cpp
// typed_behavior<Signatures...> - type-safe behavior
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

// Helper for typed actor message handling
template<typename T>
struct handler_type;

// Signature: result<R>(Args...)
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

### 4.3 Result Type

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
    result(T&& val) : has_value_(true), value_(std::move(val)) {}
    result(error err) : has_value_(false), value_(err) {}
    
    bool has_value_;
    std::variant<T, error> value_;
};

// Specialization for void results
template<>
class result<void> {
public:
    static result<void> make();
    static result<void> make(error err);
    
    bool has_value() const { return has_value_; }
    void value() const {}  // No-op for void
    const error& error() const { return error_; }
    
private:
    result<void>() : has_value_(true) {}
    result<void>(error err) : has_value_(false), error_(err) {}
    
    bool has_value_;
    error error_;
};
```

---

## 5. Supervision

### 5.1 Supervision Types

```cpp
// SupervisionDirective - what to do with failed child
enum class SupervisionDirective {
    Restart,    // Restart the failed actor
    Stop,       // Terminate permanently
    Escalate,   // Pass to parent supervisor
};

// ChildFailure - information about child failure
struct ChildFailure {
    ActorId child_id;
    error reason;
    SupervisionDirective directive;
};

// SupervisionPolicy - configuration for restart behavior
struct SupervisionPolicy {
    enum class Strategy { OneForOne, AllForOne, OneForAll };
    
    Strategy strategy = Strategy::OneForOne;
    uint32_t max_restarts = 10;
    std::chrono::milliseconds restart_interval{5000};
};
```

### 5.2 Supervisor Interface

```cpp
class Supervisor {
public:
    virtual ~Supervisor() = default;
    virtual SupervisionDirective on_child_failure(const ChildFailure& failure) = 0;
    virtual void on_child_stopped(ActorId child_id);
};

// OneForOneStrategy - only restart failed child
class OneForOneSupervisor : public Supervisor {
public:
    explicit OneForOneSupervisor(SupervisionPolicy policy = {});
    SupervisionDirective on_child_failure(const ChildFailure& failure) override;
    
private:
    SupervisionPolicy policy_;
};

// AllForOneStrategy - restart all children if any fail
class AllForOneSupervisor : public Supervisor {
public:
    explicit AllForOneSupervisor(SupervisionPolicy policy = {});
    SupervisionDirective on_child_failure(const ChildFailure& failure) override;
    
private:
    SupervisionPolicy policy_;
};
```

### 5.3 Supervisor Actor

```cpp
class supervisor_actor : public event_based_actor {
public:
    supervisor_actor(Supervisor& strategy, std::vector<Actor> children);
    
protected:
    Behavior make_behavior() override;
    
private:
    void handle_child_down(const down_msg& msg);
    void restart_child(ActorId child_id);
    void restart_all_children();
    
    Supervisor& strategy_;
    std::vector<Actor> children_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
    std::chrono::steady_clock::time_point first_failure_time_;
};
```

### 5.4 Self-Supervising Actors

```cpp
class self_supervising_actor : public event_based_actor {
public:
    void add_child(Actor child);
    void remove_child(Actor child);
    
protected:
    // Override to implement custom supervision
    virtual SupervisionDirective on_failure(ActorId child_id, const error& err);
    
private:
    void handle_child_down(const down_msg& msg);
    SupervisionDirective decide_restart(ActorId child_id, const error& err);
    
    std::vector<Actor> children_;
    SupervisionPolicy policy_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
};
```

---

## 6. Actor Lifecycle and Hibernation

### 6.1 Lifecycle States

```cpp
enum class ActorLifecycleState {
    Created,      // Constructed but not activated
    Active,      // Processing messages
    Hibernating, // Idle, state persisted
    Deactivating, // Cleanup in progress
    Dead,        // Fully cleaned up
};
```

### 6.2 Lifecycle Manager

```cpp
class ActorLifecycle {
public:
    ActorLifecycleState state() const { return state_; }
    
    // Transition guards
    bool can_activate() const;
    bool can_hibernate() const;
    bool can_deactivate() const;
    bool is_active() const { return state_ == ActorLifecycleState::Active; }
    bool is_hibernating() const { return state_ == ActorLifecycleState::Hibernating; }
    bool is_alive() const;
    
    // Transitions
    void activate();
    void hibernate();
    void deactivate();
    void mark_dead();
    
    // Idle timeout
    void set_idle_timeout(std::chrono::milliseconds timeout);
    std::chrono::milliseconds idle_timeout() const { return idle_timeout_; }
    void reset_idle_timer();
    
private:
    ActorLifecycleState state_ = ActorLifecycleState::Created;
    std::chrono::milliseconds idle_timeout_{30000};
    std::optional<AlarmHandle> idle_alarm_;
};
```

### 6.3 Hibernation Manager

```cpp
class IHibernationManager {
public:
    virtual ~IHibernationManager() = default;
    virtual Task<> save_state(ActorId id, const bytes& state) = 0;
    virtual Task<bytes> load_state(ActorId id) = 0;
    virtual Task<> delete_state(ActorId id) = 0;
};

// Actor with hibernation support
class hibernating_actor : public event_based_actor {
public:
    void set_hibernate_idle_timeout(std::chrono::milliseconds t);
    
protected:
    virtual bytes capture_state() = 0;
    virtual void restore_state(const bytes& data) = 0;
    
    void on_deactivate() override;
    void on_activate() override;
    
private:
    IHibernationManager* hibernation_manager_ = nullptr;
    std::chrono::milliseconds hibernate_timeout_{30000};
};
```

### 6.4 ActorHost

Manages actor execution on a node.

```cpp
class ActorHost {
public:
    ActorHost(ActorSystem& system, NodeId node_id);
    ~ActorHost();
    
    // Activate actor on this host
    ActorId activate_actor(ActorType type, ActorId id);
    ActorId activate_actor(ActorType type, const std::string& name);
    
    // Deactivate (optionally hibernate first)
    void deactivate_actor(ActorId id, bool hibernate = false);
    
    // Message enqueuing
    void enqueue(ActorId target, MessageVariant msg);
    
    // Actor type registration
    void register_actor_type(const ActorTypeDef& def);
    
    // Hibernation
    void set_hibernation_manager(IHibernationManager* mgr);
    
    // Node info
    NodeId node_id() const { return node_id_; }
    
private:
    struct ActorInstance {
        Actor actor;
        ActorLifecycle lifecycle;
        std::optional<AlarmHandle> idle_alarm;
    };
    
    ActorId next_actor_id();
    
    std::unordered_map<ActorId, ActorInstance> actors_;
    ActorSystem& system_;
    NodeId node_id_;
    ActorId::counter_type id_counter_;
    IHibernationManager* hibernation_manager_ = nullptr;
};
```

---

## 7. Actor References and Addresses

### 7.1 ActorAddress

Uniquely identifies an actor across the distributed system.

```cpp
struct ActorAddress {
    NodeId node_id = 0;       // Network location (0 for local)
    ActorType type = 0;      // Actor type identifier
    ActorId id;               // Unique instance ID
    uint64_t incarnation = 0; // Increments on restart
    
    bool operator==(const ActorAddress& other) const;
    bool is_local() const { return node_id == 0 || node_id == local_node_id(); }
    explicit operator bool() const { return id.value() != 0; }
    
    // Serialization for network transmission
    bytes serialize() const;
    static ActorAddress deserialize(bytes_view data);
    
private:
    static NodeId local_node_id();  // Returns this node's ID
};

// std::hash specialization for use in unordered_map/set
template<>
struct hash<ActorAddress> {
    size_t operator()(const ActorAddress& addr) const noexcept;
};

using ActorAddr = ActorAddress;

constexpr ActorAddr invalid_actor_addr{};
```

### 7.2 Actor Reference

Opaque reference to a local actor.

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

### 7.3 Typed Actor Reference

Type-safe reference to a typed actor.

```cpp
template<typename... Signatures>
class typed_actor {
public:
    using base_type = typed_event_based_actor<Signatures...>;
    
    typed_actor() = default;
    explicit typed_actor(std::shared_ptr<base_type> ptr);
    
    // Type-safe send
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

### 7.4 ActorProxy

Reference to a possibly remote actor via transport.

```cpp
class ActorProxy {
public:
    ActorProxy(ActorAddress address, Transport* transport);
    
    ActorAddress address() const { return address_; }
    bool is_local() const { return address_.is_local(); }
    
    // Invoke with response
    template<typename T>
    Task<T> invoke(T&& msg, std::chrono::milliseconds timeout = {});
    
    // Fire-and-forget
    template<typename T>
    void send(T&& msg);
    
private:
    ActorAddress address_;
    Transport* transport_;
};
```

### 7.5 Unified Reference

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

## 8. Mailbox Integration

### 8.1 ActorMailbox

Specialized mailbox for actors. Extends the existing `IMailbox<T>` interface with actor-specific features.

```cpp
// ActorBase - alias for abstract_actor (base class for all actors)
using ActorBase = abstract_actor;

template<typename T>
class ActorMailbox : public IMailbox<T> {
public:
    ActorMailbox() = default;
    
    void push(Message<T>&& msg) noexcept override;
    bool try_pop(Message<T>& out) noexcept override;
    bool pop(Message<T>& out) override;
    
    size_t size() const override;
    bool empty() const override;
    
    // Blocking pop with timeout
    bool pop_with_timeout(Message<T>& out, std::chrono::milliseconds timeout);
    
    // Owner association for scheduling
    void set_owner(ActorBase* owner);
    
private:
    mutable std::mutex mutex_;
    std::queue<Message<T>> queue_;
    std::condition_variable cv_;
    ActorBase* owner_ = nullptr;
};
```

### 8.2 Message Types

```cpp
// ActorMessage - full message with metadata
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
    down_msg,
    exit_msg,
    link_msg,
    unlink_msg
    // ... user-defined types
>;
```

---

## 9. File Structure

```
include/hpactor/
├── platform.hpp                    # Platform detection
├── message.hpp                     # Message<T> wrapper
├── mailbox.hpp                     # IMailbox<T> interface
├── mutex_mailbox.hpp               # MutexMailbox<T>
│
├── actor/
│   ├── actor_fwd.hpp               # Forward declarations
│   ├── abstract_actor.hpp          # abstract_actor
│   ├── local_actor.hpp             # local_actor
│   ├── event_based_actor.hpp       # event_based_actor
│   ├── typed_actor.hpp             # typed_actor<>, typed_event_based_actor<>
│   ├── stateful_actor.hpp          # stateful_actor<T>
│   ├── blocking_actor.hpp          # blocking_actor
│   ├── scoped_actor.hpp            # scoped_actor
│   ├── virtual_actor.hpp           # virtual_actor
│   └── broker.hpp                  # broker
│
├── actor_context.hpp               # ActorContext
├── actor_system.hpp                # ActorSystem
├── behavior.hpp                   # Behavior, message_handler
├── typed_behavior.hpp              # typed_behavior<>
├── result.hpp                     # result<T>
│
├── supervision/
│   ├── supervision.hpp             # Supervisor, directives
│   ├── one_for_one_supervisor.hpp # OneForOneStrategy
│   └── all_for_one_supervisor.hpp # AllForOneStrategy
│
├── lifecycle/
│   ├── actor_lifecycle.hpp         # ActorLifecycle
│   ├── hibernation.hpp             # IHibernationManager
│   └── actor_host.hpp              # ActorHost
│
└── ref/
    ├── actor_address.hpp           # ActorAddress
    ├── actor_ref.hpp               # Actor, ActorRef
    ├── typed_actor_ref.hpp        # typed_actor<>
    └── actor_proxy.hpp             # ActorProxy
```

---

## 10. Implementation Phases

### Phase A: Core Actor (Week 1)
- `abstract_actor`, `local_actor`, `event_based_actor`
- `Behavior`, `message_handler`
- Basic `ActorSystem` and `ActorContext`
- Unit tests

### Phase B: Typed Actors (Week 2)
- `typed_event_based_actor<Signatures...>`
- `typed_behavior<Signatures...>`
- `result<T>`
- Typed actor spawn factory

### Phase C: Supervision (Week 3)
- `Supervisor`, `OneForOneSupervisor`, `AllForOneSupervisor`
- `supervisor_actor`
- `down_msg`, `exit_msg` handling
- Link/unlink/monitor

### Phase D: Stateful & Blocking Actors (Week 4)
- `stateful_actor<T>`
- `blocking_actor`, `scoped_actor`
- `receive()` implementations

### Phase E: Lifecycle & Hibernation (Week 5)
- `ActorLifecycle` state machine
- `ActorHost`
- `IHibernationManager`
- `hibernating_actor`

### Phase F: References & Proxy (Week 6)
- `ActorAddress`, `ActorRef`
- `ActorProxy` for remote actors
- Type-safe handles

---

## 11. Open Questions

- [ ] What is the default idle timeout before hibernation?
- [ ] Should blocking actors be spawned with explicit thread affinity?
- [ ] Maximum number of children per supervisor?
- [ ] What error types should be defined by default?

---

## 12. References

- [CAF Actor Types](actor/Actors.rst)
- [Distributed Actor System Architecture](../Distributed%20Actor%20System%20Architecture.md)
- [Previous Actor Framework Design](../2026-04-10-cpp-actor-framework-design.md)
