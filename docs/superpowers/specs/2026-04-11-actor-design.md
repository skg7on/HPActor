# C++ Actor Framework - Actor Design Specification

> **Spec Status:** Draft
> **Created:** 2026-04-11
> **Based on:** Distributed Actor System Architecture.md,

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

Core types used throughout the actor framework.

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

// Incarnation - incremented on actor restart
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
constexpr uint32_t user = 1000;
} // namespace errors

// Clock - for time-based operations
class Clock {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::milliseconds;
    time_point now() const;
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

// TraceContext - for distributed tracing
struct TraceContext {
    uint64_t trace_id = 0;
    uint64_t span_id = 0;
    uint8_t flags = 0;
};

// bytes - raw byte buffer
using bytes = std::vector<uint8_t>;

// Task<T> - coroutine-based async task
template<typename T>
class Task;
```

---

## 3. Actor Type Hierarchy

```
abstract_actor
└── local_actor
    ├── event_based_actor
    │   ├── typed_event_based_actor<Signatures...>
    │   ├── stateful_actor<T>
    │   └── hibernating_actor
    ├── blocking_actor
    │   └── scoped_actor
    └── broker
```

### 3.1 abstract_actor

Base class for all actors.

```cpp
class abstract_actor : public std::enable_shared_from_this<abstract_actor> {
public:
    virtual ~abstract_actor() = default;
    ActorId id() const { return id_; }
    ActorAddress address() const;
    ActorSystem& system() { return system_; }
    void link_to(ActorAddr other);
    void unlink_from(ActorAddr other);
    void monitor(const ActorAddr& target);
    void demonitor(const ActorAddr& target);
    template<typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);
    virtual void receive(MessageVariant&& msg) = 0;
protected:
    abstract_actor(ActorId id, ActorSystem& sys);
private:
    ActorId id_;
    ActorSystem& system_;
};
```

### 3.2 local_actor

Base for locally executed actors.

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

### 3.3 event_based_actor

Cooperatively scheduled actor.

```cpp
class event_based_actor : public local_actor {
public:
    void become(Behavior bh);
    void become_empty();
    void receive(MessageVariant&& msg) override;
protected:
    virtual Behavior make_behavior() { return {}; }
    virtual void on_activate();
    virtual void on_deactivate();
    virtual void on_exit() {}
private:
    Behavior behavior_;
};
```

### 3.4 typed_event_based_actor

Statically typed actor.

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

Example:
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

### 3.5 stateful_actor

Actor with explicit state.

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

### 3.6 blocking_actor

Actor with blocking receive.

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
    virtual void on_activate();
    virtual void on_deactivate();
private:
    error fail_state_;
};
```

### 3.7 scoped_actor

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

### 3.8 Additional Types

```cpp
// virtual_actor - Orleans-style (for distributed)
class virtual_actor : public abstract_actor {
public:
    virtual Task<> on_activation();
    virtual Task<> on_passivation();
    virtual Task<> save_state() = 0;
    virtual Task<> load_state() = 0;
};

// broker - for middleware/proxy
class broker : public event_based_actor {
public:
    void relay(MessageVariant&& msg);
    std::vector<Actor> peers() const;
};
```

---

## 4. ActorContext and ActorSystem

### 4.1 ActorContext

```cpp
class ActorContext {
public:
    ActorContext(Actor owner);
    ~ActorContext();
    template<typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);
    template<typename T, typename... Args>
    T spawn(Args&&... args);
    void send(const ActorAddress& target, MessageVariant msg);
    void reply(MessageVariant msg);
    void reply_with_error(error err);
    void schedule(std::chrono::milliseconds delay, MessageVariant msg);
    std::vector<Actor> children() const;
    void add_child(Actor child);
    void remove_child(Actor child);
    std::vector<ActorAddress> linked_actors() const;
    void monitor(const ActorAddress& target);
private:
    Actor owner_;
    std::vector<Actor> children_;
    std::vector<ActorAddress> linked_;
    std::vector<ActorAddress> monitored_;
};
```

### 4.2 ActorSystem

```cpp
class ActorSystem {
public:
    explicit ActorSystem(const Config& config);
    ~ActorSystem();
    template<typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);
    template<typename T, typename... Args>
    T spawn(Args&&... args);
    void register_actor(const std::string& name, Actor actor);
    Actor resolve_actor(const std::string& name);
    void unregister_actor(const std::string& name);
    void register_actor_type(const ActorTypeDef& def);
    ActorTypeDef get_actor_type(ActorType type) const;
    Clock& clock() { return clock_; }
    Actor system_actor() { return system_actor_; }
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

## 5. Behavior and Message Handling

### 5.1 Behavior

```cpp
class Behavior {
public:
    Behavior() = default;
    template<typename... Handlers>
    Behavior(Handlers&&... handlers);
    result<MessageVariant> invoke(MessageVariant& msg);
    bool matches(const MessageVariant& msg) const;
    Behavior or_else(const Behavior& other) const;
    template<typename T>
    void add(T&& type_tag, message_handler handler);
private:
    std::vector<message_handler> handlers_;
};

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

### 5.2 Typed Behavior

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
    template<typename F>
    result operator()(F&& f, Args... args) { return f(args...); }
};
```

### 5.3 Result Type

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

template<>
class result<void> {
public:
    static result<void> make();
    static result<void> make(error err);
    bool has_value() const { return has_value_; }
    void value() const {}
    const error& error() const { return error_; }
private:
    result<void>() : has_value_(true) {}
    result<void>(error err) : has_value_(false), error_(err) {}
    bool has_value_;
    error error_;
};
```

---

## 6. Supervision

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

class self_supervising_actor : public event_based_actor {
public:
    void add_child(Actor child);
    void remove_child(Actor child);
protected:
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

## 7. Actor Lifecycle and Hibernation

```cpp
enum class ActorLifecycleState {
    Created, Active, Hibernating, Deactivating, Dead
};

class ActorLifecycle {
public:
    ActorLifecycleState state() const { return state_; }
    bool can_activate() const;
    bool can_hibernate() const;
    bool can_deactivate() const;
    bool is_active() const { return state_ == ActorLifecycleState::Active; }
    bool is_hibernating() const { return state_ == ActorLifecycleState::Hibernating; }
    bool is_alive() const;
    void activate();
    void hibernate();
    void deactivate();
    void mark_dead();
    void set_idle_timeout(std::chrono::milliseconds timeout);
    std::chrono::milliseconds idle_timeout() const { return idle_timeout_; }
    void reset_idle_timer();
private:
    ActorLifecycleState state_ = ActorLifecycleState::Created;
    std::chrono::milliseconds idle_timeout_{30000};
    std::optional<AlarmHandle> idle_alarm_;
};

class IHibernationManager {
public:
    virtual ~IHibernationManager() = default;
    virtual Task<> save_state(ActorId id, const bytes& state) = 0;
    virtual Task<bytes> load_state(ActorId id) = 0;
    virtual Task<> delete_state(ActorId id) = 0;
};

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

class ActorHost {
public:
    ActorHost(ActorSystem& system, NodeId node_id);
    ~ActorHost();
    ActorId activate_actor(ActorType type, ActorId id);
    ActorId activate_actor(ActorType type, const std::string& name);
    void deactivate_actor(ActorId id, bool hibernate = false);
    void enqueue(ActorId target, MessageVariant msg);
    void register_actor_type(const ActorTypeDef& def);
    void set_hibernation_manager(IHibernationManager* mgr);
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

## 8. Actor References and Addresses

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

template<>
struct hash<ActorAddress> {
    size_t operator()(const ActorAddress& addr) const noexcept;
};

using ActorAddr = ActorAddress;
constexpr ActorAddr invalid_actor_addr{};

// Actor reference (opaque handle to local actor)
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

// Typed actor reference
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

// ActorProxy - reference to possibly remote actor
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

// Unified reference
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

## 9. Mailbox Integration

```cpp
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
    bool pop_with_timeout(Message<T>& out, std::chrono::milliseconds timeout);
    void set_owner(ActorBase* owner);
private:
    mutable std::mutex mutex_;
    std::queue<Message<T>> queue_;
    std::condition_variable cv_;
    ActorBase* owner_ = nullptr;
};

struct ActorMessage {
    MessageId id;
    ActorAddress sender;
    ActorAddress receiver;
    std::string method_name;
    MessageVariant payload;
    bool is_one_way = false;
    TraceContext trace;
};

struct down_msg { ActorAddress terminated_actor; error reason; };
struct exit_msg { ActorAddress sender; error reason; };
struct link_msg { ActorAddress target; };
struct unlink_msg { ActorAddress target; };

using MessageVariant = std::variant<down_msg, exit_msg, link_msg, unlink_msg /* ... */>;
```

---

## 10. File Structure

```
include/hpactor/
├── platform.hpp
├── message.hpp
├── mailbox.hpp
├── mutex_mailbox.hpp
├── actor/
│   ├── actor_fwd.hpp
│   ├── abstract_actor.hpp
│   ├── local_actor.hpp
│   ├── event_based_actor.hpp
│   ├── typed_actor.hpp
│   ├── stateful_actor.hpp
│   ├── blocking_actor.hpp
│   ├── scoped_actor.hpp
│   ├── virtual_actor.hpp
│   └── broker.hpp
├── actor_context.hpp
├── actor_system.hpp
├── behavior.hpp
├── typed_behavior.hpp
├── result.hpp
├── supervision/
│   ├── supervision.hpp
│   ├── one_for_one_supervisor.hpp
│   └── all_for_one_supervisor.hpp
├── lifecycle/
│   ├── actor_lifecycle.hpp
│   ├── hibernation.hpp
│   └── actor_host.hpp
└── ref/
    ├── actor_address.hpp
    ├── actor_ref.hpp
    ├── typed_actor_ref.hpp
    └── actor_proxy.hpp
```

---

## 11. Implementation Phases

| Phase | Components | Status |
|-------|------------|--------|
| Phase A | Core Actor | NEXT |
| Phase B | Typed Actors | Pending |
| Phase C | Supervision | Pending |
| Phase D | Stateful & Blocking | Pending |
| Phase E | Lifecycle & Hibernation | Pending |
| Phase F | References & Proxy | Pending |

---

## 12. References

- [Distributed Actor System Architecture](../Distributed%20Actor%20System%20Architecture.md)
- [CAF Actors.rst](../../architecture/actor/Actors.rst)
- [Previous Framework Design](../2026-04-10-cpp-actor-framework-design.md)
