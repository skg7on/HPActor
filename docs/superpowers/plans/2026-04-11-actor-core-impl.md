# Actor Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the core actor framework based on the actor design specification, covering fundamental types, actor hierarchy, behavior system, ActorContext, ActorSystem, and mailbox integration.

**Architecture:** Event-based actors with cooperative scheduling. The design follows the CAF (C++ Actor Framework) model with statically typed and dynamically typed messaging, hierarchical supervision, and explicit lifecycle with optional hibernation. Implementation proceeds in dependency order: fundamental types first, then actor base classes, then behavior/messaging, then system integration.

**Tech Stack:** C++20, standard library only (no external dependencies except where noted), CMake build system.

---

## Phase A: Fundamental Types

### A.1: Core Type Definitions

**Files:**
- Create: `include/hpactor/types.hpp`
- Create: `include/hpactor/types_fwd.hpp`
- Create: `tests/test_types.cpp`

- [ ] **Step 1: Write failing test for ActorId**

```cpp
// tests/test_types.cpp
#include <hpactor/types.hpp>
#include <cstdint>
#include <utility>

void test_actor_id_default() {
    hpactor::ActorId id;
    assert(id.value() == 0);
    assert(id == hpactor::ActorId{});
}

void test_actor_id_explicit() {
    hpactor::ActorId id(42);
    assert(id.value() == 42);
    assert(id != hpactor::ActorId{});
}

void test_actor_id_equality() {
    hpactor::ActorId a(1), b(1), c(2);
    assert(a == b);
    assert(!(a == c));
}
```

- [ ] **Step 2: Run test to verify it fails**
Run: `cd build && cmake .. -DCMAKE_CXX_COMPILER=clang++ && make test_types 2>&1 | head -20`
Expected: FAIL - header not found

- [ ] **Step 3: Create types_fwd.hpp with forward declarations**

```cpp
// include/hpactor/types_fwd.hpp
#pragma once
#include <cstdint>

namespace hpactor {

struct ActorId;
using NodeId = uint32_t;
constexpr NodeId InvalidNodeId = 0;
using ActorType = uint32_t;
constexpr ActorType InvalidActorType = 0;
using incarnation_type = uint64_t;

struct MessageId;

class error;
namespace errors {
constexpr uint32_t unknown = 1;
constexpr uint32_t actor_down = 2;
constexpr uint32_t actor_not_found = 3;
constexpr uint32_t mailbox_full = 4;
constexpr uint32_t timeout = 5;
constexpr uint32_t user = 1000;
} // namespace errors

class Clock;
class AlarmHandle;

struct TraceContext;

using bytes = std::vector<uint8_t>;

template<typename T>
class Task;

} // namespace hpactor
```

- [ ] **Step 4: Create types.hpp with full type definitions**

```cpp
// include/hpactor/types.hpp
#pragma once
#include "types_fwd.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace hpactor {

// ActorId - unique identifier for an actor instance
struct ActorId {
    using counter_type = uint64_t;
    ActorId() = default;
    explicit ActorId(counter_type value) : value_(value) {}
    counter_type value() const { return value_; }
    bool operator==(const ActorId& other) const { return value_ == other.value_; }
    bool operator!=(const ActorId& other) const { return !(*this == other); }
private:
    counter_type value_ = 0;
};

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

// Clock - for time-based operations
class Clock {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::milliseconds;
    time_point now() const { return std::chrono::steady_clock::now(); }
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

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_types && ./test_types`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/types.hpp include/hpactor/types_fwd.hpp tests/test_types.cpp
git commit -m "feat: add fundamental types (ActorId, error, Clock, etc.)"
```

---

### A.2: Result Type

**Files:**
- Modify: `include/hpactor/types.hpp` (add result<T> class)
- Create: `tests/test_result.cpp`

- [ ] **Step 1: Write failing test for result<T>**

```cpp
// tests/test_result.cpp
#include <hpactor/types.hpp>
#include <cassert>

void test_result_value() {
    auto r = hpactor::result<int>::make(42);
    assert(r.has_value());
    assert(r.value() == 42);
}

void test_result_error() {
    auto r = hpactor::result<int>::make(hpactor::error{1, "test"});
    assert(!r.has_value());
    assert(r.error().code() == 1);
}

void test_result_void_success() {
    auto r = hpactor::result<void>::make();
    assert(r.has_value());
}

void test_result_void_error() {
    auto r = hpactor::result<void>::make(hpactor::error{1});
    assert(!r.has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**
Run: `cd build && make test_result 2>&1 | head -20`
Expected: FAIL - result not defined

- [ ] **Step 3: Add result<T> to types.hpp**

```cpp
// Add to types.hpp before closing namespace

// Result type for message handlers
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
    void value() const {}  // No-op for void
    const error& error() const { return error_; }

private:
    result<void>() : has_value_(true) {}
    result<void>(error err) : has_value_(false), error_(err) {}

    bool has_value_;
    error error_;
};
```

- [ ] **Step 4: Run test to verify it passes**
Run: `cd build && make test_result && ./test_result`
Expected: PASS

- [ ] **Step 5: Commit**
```bash
git add include/hpactor/types.hpp tests/test_result.cpp
git commit -m "feat: add result<T> type for message handler returns"
```

---

## Phase B: Actor Base Classes

### B.1: Actor Forward Declarations and ActorAddress

**Files:**
- Create: `include/hpactor/actor/actor_fwd.hpp`
- Create: `include/hpactor/ref/actor_address.hpp`
- Create: `tests/test_actor_address.cpp`

- [ ] **Step 1: Write failing test for ActorAddress**

```cpp
// tests/test_actor_address.cpp
#include <hpactor/ref/actor_address.hpp>
#include <cassert>

void test_actor_address_default() {
    hpactor::ActorAddress addr;
    assert(!addr);
    assert(addr.id().value() == 0);
}

void test_actor_address_local() {
    hpactor::ActorId id(1);
    hpactor::ActorAddress addr{hpactor::InvalidNodeId, 0, id, 0};
    assert(addr.is_local());
}

void test_actor_address_equality() {
    hpactor::ActorId id1(1), id2(1), id3(2);
    hpactor::ActorAddress a{hpactor::InvalidNodeId, 0, id1, 0};
    hpactor::ActorAddress b{hpactor::InvalidNodeId, 0, id2, 0};
    hpactor::ActorAddress c{hpactor::InvalidNodeId, 0, id3, 0};
    assert(a == b);
    assert(!(a == c));
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create actor_fwd.hpp**

```cpp
// include/hpactor/actor/actor_fwd.hpp
#pragma once
#include "../../types_fwd.hpp"

namespace hpactor {

class abstract_actor;
class local_actor;
class event_based_actor;
class blocking_actor;
class scoped_actor;

template<typename... Signatures>
class typed_event_based_actor;

template<typename T>
class stateful_actor;

class Actor;
class ActorRef;

} // namespace hpactor
```

- [ ] **Step 4: Create actor_address.hpp**

```cpp
// include/hpactor/ref/actor_address.hpp
#pragma once
#include "../types.hpp"
#include <functional>
#include <string>

namespace hpactor {

// ActorAddress - uniquely identifies an actor across the distributed system
struct ActorAddress {
    NodeId node_id = 0;       // Network location (0 for local)
    ActorType type = 0;      // Actor type identifier
    ActorId id;               // Unique instance ID
    uint64_t incarnation = 0; // Increments on restart

    ActorAddress() = default;
    ActorAddress(NodeId node, ActorType t, ActorId i, uint64_t inc)
        : node_id(node), type(t), id(i), incarnation(inc) {}

    bool operator==(const ActorAddress& other) const {
        return node_id == other.node_id &&
               type == other.type &&
               id == other.id &&
               incarnation == other.incarnation;
    }
    bool operator!=(const ActorAddress& other) const { return !(*this == other); }
    bool is_local() const { return node_id == InvalidNodeId; }
    explicit operator bool() const { return id.value() != 0; }
};

using ActorAddr = ActorAddress;
constexpr ActorAddr invalid_actor_addr{};

} // namespace hpactor

// std::hash specialization
template<>
struct std::hash<hpactor::ActorAddress> {
    size_t operator()(const hpactor::ActorAddress& addr) const noexcept {
        size_t h = 0;
        hpactor::hash_combine(h, addr.node_id);
        hpactor::hash_combine(h, addr.type);
        hpactor::hash_combine(h, addr.id.value());
        hpactor::hash_combine(h, addr.incarnation);
        return h;
    }
private:
    static void hash_combine(size_t& seed, auto value) {
        seed ^= std::hash<decltype(value)>{}(value) + 0x9e3779b9 + (seed<<6) + (seed>>2);
    }
};
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_actor_address && ./test_actor_address`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/actor/actor_fwd.hpp include/hpactor/ref/actor_address.hpp tests/test_actor_address.cpp
git commit -m "feat: add ActorAddress and actor forward declarations"
```

---

### B.2: Actor Ref (Actor handle)

**Files:**
- Create: `include/hpactor/ref/actor_ref.hpp`
- Create: `tests/test_actor_ref.cpp`

- [ ] **Step 1: Write failing test for Actor**

```cpp
// tests/test_actor_ref.cpp
#include <hpactor/ref/actor_ref.hpp>
#include <cassert>

void test_actor_default() {
    hpactor::Actor actor;
    assert(!actor);
    assert(actor.id().value() == 0);
}

void test_actor_conversion_to_address() {
    // ActorAddress conversion tested via id()
    hpactor::Actor actor;
    hpactor::ActorAddress addr = actor.address();
    assert(!addr);
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create actor_ref.hpp**

```cpp
// include/hpactor/ref/actor_ref.hpp
#pragma once
#include "actor_address.hpp"
#include <memory>

namespace hpactor {

class abstract_actor;

// Actor - opaque reference to a local actor
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

} // namespace hpactor
```

- [ ] **Step 4: Create actor_ref.cpp stub**

```cpp
// src/actor_ref.cpp
#include <hpactor/ref/actor_ref.hpp>

namespace hpactor {

Actor::Actor(std::shared_ptr<abstract_actor> ptr) : actor_(std::move(ptr)) {}

ActorId Actor::id() const {
    return actor_ ? actor_->id() : ActorId{};
}

ActorType Actor::type() const {
    return actor_ ? actor_->type() : InvalidActorType;
}

ActorAddress Actor::address() const {
    return actor_ ? actor_->address() : ActorAddress{};
}

Actor::operator ActorAddress() const { return address(); }
Actor::operator ActorAddr() const { return address(); }
Actor::explicit operator bool() const { return actor_ != nullptr; }

void Actor::swap(Actor& other) noexcept { actor_.swap(other.actor_); }

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_actor_ref && ./test_actor_ref`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/ref/actor_ref.hpp src/actor_ref.cpp tests/test_actor_ref.cpp
git commit -m "feat: add Actor reference handle"
```

---

### B.3: abstract_actor

**Files:**
- Create: `include/hpactor/actor/abstract_actor.hpp`
- Create: `tests/test_abstract_actor.cpp`

- [ ] **Step 1: Write failing test for abstract_actor**

```cpp
// tests/test_abstract_actor.cpp
#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor_system.hpp>
#include <cassert>

void test_abstract_actor_interface() {
    // Abstract actor cannot be instantiated directly
    // Test that it has the required virtual interface
    static_assert(sizeof(hpactor::abstract_actor) > 0, "abstract_actor should not be empty");
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create abstract_actor.hpp**

```cpp
// include/hpactor/actor/abstract_actor.hpp
#pragma once
#include "actor_fwd.hpp"
#include "../ref/actor_address.hpp"
#include "../types.hpp"
#include <memory>

namespace hpactor {

class ActorSystem;

// abstract_actor - base class for all actors
class abstract_actor : public std::enable_shared_from_this<abstract_actor> {
public:
    virtual ~abstract_actor() = default;

    ActorId id() const { return id_; }
    ActorType type() const { return type_; }
    ActorAddress address() const { return address_; }
    ActorSystem& system() { return system_; }
    const ActorSystem& system() const { return system_; }

    // Linking - death sharing
    void link_to(const ActorAddr& other);
    void unlink_from(const ActorAddr& other);

    // Monitoring - receive down messages
    void monitor(const ActorAddr& target);
    void demonitor(const ActorAddr& target);

    // Receive message (called by scheduler)
    virtual void receive(MessageVariant&& msg) = 0;

protected:
    abstract_actor(ActorId id, ActorType type, ActorSystem& sys);

private:
    ActorId id_;
    ActorType type_;
    ActorSystem& system_;
    ActorAddress address_;
};

// MessageVariant - std::variant for all message types
using MessageVariant = std::variant<
    down_msg,
    exit_msg,
    link_msg,
    unlink_msg
    // ... user-defined types
>;

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

} // namespace hpactor
```

- [ ] **Step 4: Run test to verify it passes**
Run: `cd build && make test_abstract_actor && ./test_abstract_actor`
Expected: PASS

- [ ] **Step 5: Commit**
```bash
git add include/hpactor/actor/abstract_actor.hpp tests/test_abstract_actor.cpp
git commit -m "feat: add abstract_actor base class"
```

---

### B.4: local_actor and event_based_actor

**Files:**
- Create: `include/hpactor/actor/local_actor.hpp`
- Create: `include/hpactor/actor/event_based_actor.hpp`
- Create: `tests/test_event_based_actor.cpp`

- [ ] **Step 1: Write failing test for local_actor and event_based_actor**

```cpp
// tests/test_event_based_actor.cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <cassert>
#include <memory>

// Simple concrete actor for testing
class test_actor : public hpactor::event_based_actor {
public:
    test_actor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::event_based_actor(ctx, sys) {}

protected:
    hpactor::Behavior make_behavior() override {
        return {
            [](int x) -> int { return x * 2; }
        };
    }
};

void test_event_based_actor_become() {
    // Test that event_based_actor can change behavior
    static_assert(sizeof(hpactor::event_based_actor) > 0, "should not be empty");
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create local_actor.hpp**

```cpp
// include/hpactor/actor/local_actor.hpp
#pragma once
#include "abstract_actor.hpp"

namespace hpactor {

class ActorContext;

// local_actor - base for locally executed actors
class local_actor : public abstract_actor {
public:
    ActorContext* context() { return ctx_; }
    ActorSystem& home_system() { return system(); }

protected:
    local_actor(ActorContext* ctx, ActorSystem& sys);
    local_actor(ActorId id, ActorContext* ctx, ActorSystem& sys);

private:
    ActorContext* ctx_ = nullptr;
};

} // namespace hpactor
```

- [ ] **Step 4: Create event_based_actor.hpp**

```cpp
// include/hpactor/actor/event_based_actor.hpp
#pragma once
#include "local_actor.hpp"
#include "../behavior.hpp"

namespace hpactor {

// event_based_actor - cooperatively scheduled actor with behavior-based message handling
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

    event_based_actor(ActorContext* ctx, ActorSystem& sys);

private:
    Behavior behavior_;
};

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_event_based_actor && ./test_event_based_actor`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/actor/local_actor.hpp include/hpactor/actor/event_based_actor.hpp tests/test_event_based_actor.cpp
git commit -m "feat: add local_actor and event_based_actor"
```

---

## Phase C: Behavior System

### C.1: Behavior and message_handler

**Files:**
- Create: `include/hpactor/behavior.hpp`
- Create: `tests/test_behavior.cpp`

- [ ] **Step 1: Write failing test for Behavior**

```cpp
// tests/test_behavior.cpp
#include <hpactor/behavior.hpp>
#include <cassert>
#include <string>

void test_behavior_empty() {
    hpactor::Behavior bh;
    assert(!bh.matches(...)); // No messages match empty behavior
}

void test_behavior_handler() {
    bool called = false;
    hpactor::Behavior bh{
        [](int x) -> int { return x * 2; }
    };
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create behavior.hpp**

```cpp
// include/hpactor/behavior.hpp
#pragma once
#include "../types.hpp"
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace hpactor {

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

} // namespace hpactor
```

- [ ] **Step 4: Create behavior.cpp**

```cpp
// src/behavior.cpp
#include <hpactor/behavior.hpp>
#include <stdexcept>

namespace hpactor {

message_handler::message_handler(T&& type_tag, F&& func)
    : func_(std::forward<F>(func)), type_(typeid(T)) {}

result<MessageVariant> message_handler::operator()(MessageVariant& msg) {
    if (!matches(msg)) {
        return result<MessageVariant>::make(error{1, "handler mismatch"});
    }
    // Type-erased invocation - would use std::visit in real implementation
    return func_(msg);
}

bool message_handler::matches(const MessageVariant& msg) const {
    return std::holds_alternative<T>(msg);
}

template<typename... Handlers>
Behavior::Behavior(Handlers&&... handlers) {
    (add(handlers, message_handler{...}), ...); // Fold expression
}

result<MessageVariant> Behavior::invoke(MessageVariant& msg) {
    for (auto& handler : handlers_) {
        if (handler.matches(msg)) {
            return handler(msg);
        }
    }
    return result<MessageVariant>::make(error{1, "no handler found"});
}

bool Behavior::matches(const MessageVariant& msg) const {
    for (const auto& handler : handlers_) {
        if (handler.matches(msg)) {
            return true;
        }
    }
    return false;
}

Behavior Behavior::or_else(const Behavior& other) const {
    Behavior result = *this;
    for (auto& h : other.handlers_) {
        result.handlers_.push_back(std::move(h));
    }
    return result;
}

template<typename T>
void Behavior::add(T&& type_tag, message_handler handler) {
    handlers_.push_back(std::move(handler));
}

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_behavior && ./test_behavior`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/behavior.hpp src/behavior.cpp tests/test_behavior.cpp
git commit -m "feat: add Behavior and message_handler"
```

---

## Phase D: ActorContext and ActorSystem

### D.1: ActorContext

**Files:**
- Create: `include/hpactor/actor_context.hpp`
- Create: `src/actor_context.cpp`
- Create: `tests/test_actor_context.cpp`

- [ ] **Step 1: Write failing test for ActorContext**

```cpp
// tests/test_actor_context.cpp
#include <hpactor/actor_context.hpp>
#include <cassert>

void test_actor_context_children() {
    // ActorContext manages children
    static_assert(sizeof(hpactor::ActorContext) > 0, "should not be empty");
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create actor_context.hpp**

```cpp
// include/hpactor/actor_context.hpp
#pragma once
#include "actor/actor_fwd.hpp"
#include "ref/actor_address.hpp"
#include "types.hpp"
#include <chrono>
#include <vector>

namespace hpactor {

class ActorContext {
public:
    ActorContext(Actor owner);
    ~ActorContext();

    // Spawn child actors
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

} // namespace hpactor
```

- [ ] **Step 4: Create actor_context.cpp**

```cpp
// src/actor_context.cpp
#include <hpactor/actor_context.hpp>

namespace hpactor {

ActorContext::ActorContext(Actor owner) : owner_(std::move(owner)) {}
ActorContext::~ActorContext() = default;

std::vector<Actor> ActorContext::children() const { return children_; }
void ActorContext::add_child(Actor child) { children_.push_back(std::move(child)); }
void ActorContext::remove_child(Actor child) {
    children_.erase(std::remove(children_.begin(), children_.end(), child), children_.end());
}

std::vector<ActorAddress> ActorContext::linked_actors() const { return linked_; }
void ActorContext::monitor(const ActorAddress& target) { monitored_.push_back(target); }

void ActorContext::send(const ActorAddress& target, MessageVariant msg) {
    // Forward to ActorSystem's registry/transport
    system().send(target, std::move(msg));
}

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_actor_context && ./test_actor_context`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/actor_context.hpp src/actor_context.cpp tests/test_actor_context.cpp
git commit -m "feat: add ActorContext for actor execution context"
```

---

### D.2: ActorSystem

**Files:**
- Create: `include/hpactor/actor_system.hpp`
- Create: `src/actor_system.cpp`
- Create: `tests/test_actor_system.cpp`

- [ ] **Step 1: Write failing test for ActorSystem**

```cpp
// tests/test_actor_system.cpp
#include <hpactor/actor_system.hpp>
#include <cassert>

void test_actor_system_default_construct() {
    // ActorSystem requires Config - test that it's non-copyable
    static_assert(!std::is_copy_constructible_v<hpactor::ActorSystem>);
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create actor_system.hpp**

```cpp
// include/hpactor/actor_system.hpp
#pragma once
#include "types.hpp"
#include "actor_context.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor {

struct Config {
    size_t scheduler_threads = 4;
    size_t max_queue_depth = 1024;
};

struct ActorTypeDef {
    std::string name;
    ActorType id;
};

class actor_registry;

// ActorSystem - the actor environment
class ActorSystem {
public:
    explicit ActorSystem(const Config& config);
    ~ActorSystem();

    // Spawn actors at system level
    template<typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);

    template<typename T, typename... Args>
    T spawn(Args&&... args);

    // Actor registry
    void register_actor(const std::string& name, Actor actor);
    Actor resolve_actor(const std::string& name);
    void unregister_actor(const std::string& name);

    // Actor type registration
    void register_actor_type(const ActorTypeDef& def);
    ActorTypeDef get_actor_type(ActorType type) const;

    // Clock
    Clock& clock() { return clock_; }

    // System actor
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

} // namespace hpactor
```

- [ ] **Step 4: Create actor_system.cpp**

```cpp
// src/actor_system.cpp
#include <hpactor/actor_system.hpp>
#include <hpactor/actor_registry.hpp>

namespace hpactor {

ActorSystem::ActorSystem(const Config& config)
    : config_(config), registry_(/* node_id = */ 0) {}

ActorSystem::~ActorSystem() = default;

void ActorSystem::register_actor(const std::string& name, Actor actor) {
    registry_.put(name, actor.address());
}

Actor ActorSystem::resolve_actor(const std::string& name) {
    auto addr = registry_.get(name);
    // Return Actor handle (would need to look up in local map)
    return Actor{};
}

void ActorSystem::unregister_actor(const std::string& name) {
    registry_.erase(name);
}

void ActorSystem::register_actor_type(const ActorTypeDef& def) {
    actor_types_[def.id] = def;
}

ActorTypeDef ActorSystem::get_actor_type(ActorType type) const {
    auto it = actor_types_.find(type);
    if (it != actor_types_.end()) {
        return it->second;
    }
    return {};
}

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_actor_system && ./test_actor_system`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/actor_system.hpp src/actor_system.cpp tests/test_actor_system.cpp
git commit -m "feat: add ActorSystem"
```

---

## Phase E: Blocking and Stateful Actors

### E.1: blocking_actor and scoped_actor

**Files:**
- Create: `include/hpactor/actor/blocking_actor.hpp`
- Create: `include/hpactor/actor/scoped_actor.hpp`
- Create: `tests/test_blocking_actor.cpp`

- [ ] **Step 1: Write failing test**

```cpp
// tests/test_blocking_actor.cpp
#include <hpactor/actor/blocking_actor.hpp>
#include <cassert>

void test_blocking_actor_interface() {
    static_assert(sizeof(hpactor::blocking_actor) > 0, "should not be empty");
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create blocking_actor.hpp**

```cpp
// include/hpactor/actor/blocking_actor.hpp
#pragma once
#include "local_actor.hpp"

namespace hpactor {

// blocking_actor - actor running in its own thread with blocking receive
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
    virtual void on_activate() override;
    virtual void on_deactivate() override;

private:
    error fail_state_;
};

} // namespace hpactor
```

- [ ] **Step 4: Create scoped_actor.hpp**

```cpp
// include/hpactor/actor/scoped_actor.hpp
#pragma once
#include "blocking_actor.hpp"

namespace hpactor {

// scoped_actor - blocking actor for non-actor contexts
class scoped_actor : public blocking_actor {
public:
    explicit scoped_actor(ActorSystem& sys);
    ~scoped_actor();

    template<typename T>
    T receive();
};

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_blocking_actor && ./test_blocking_actor`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/actor/blocking_actor.hpp include/hpactor/actor/scoped_actor.hpp tests/test_blocking_actor.cpp
git commit -m "feat: add blocking_actor and scoped_actor"
```

---

### E.2: stateful_actor<T>

**Files:**
- Create: `include/hpactor/actor/stateful_actor.hpp`
- Create: `tests/test_stateful_actor.cpp`

- [ ] **Step 1: Write failing test**

```cpp
// tests/test_stateful_actor.cpp
#include <hpactor/actor/stateful_actor.hpp>
#include <cassert>

struct counter_state {
    int value = 0;
};

void test_stateful_actor_state_access() {
    // stateful_actor manages state
    static_assert(sizeof(hpactor::stateful_actor<counter_state>) > 0, "should not be empty");
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create stateful_actor.hpp**

```cpp
// include/hpactor/actor/stateful_actor.hpp
#pragma once
#include "event_based_actor.hpp"

namespace hpactor {

// stateful_actor - actor with explicit state
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

} // namespace hpactor
```

- [ ] **Step 4: Run test to verify it passes**
Run: `cd build && make test_stateful_actor && ./test_stateful_actor`
Expected: PASS

- [ ] **Step 5: Commit**
```bash
git add include/hpactor/actor/stateful_actor.hpp tests/test_stateful_actor.cpp
git commit -m "feat: add stateful_actor<T>"
```

---

## Phase F: Typed Actors

### F.1: typed_event_based_actor and typed_behavior

**Files:**
- Create: `include/hpactor/actor/typed_actor.hpp`
- Create: `include/hpactor/typed_behavior.hpp`
- Create: `tests/test_typed_actor.cpp`

- [ ] **Step 1: Write failing test**

```cpp
// tests/test_typed_actor.cpp
#include <hpactor/actor/typed_actor.hpp>
#include <cassert>

using calculator_actor = hpactor::typed_actor<
    hpactor::result<int>(add, int, int),
    hpactor::result<int>(subtract, int, int),
    hpactor::result<void>(shutdown)
>;

void test_typed_actor_definition() {
    static_assert(sizeof(calculator_actor) > 0, "typed_actor should be instantiable");
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - header not found

- [ ] **Step 3: Create typed_behavior.hpp**

```cpp
// include/hpactor/typed_behavior.hpp
#pragma once
#include "behavior.hpp"
#include <tuple>

namespace hpactor {

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

} // namespace hpactor
```

- [ ] **Step 4: Create typed_actor.hpp**

```cpp
// include/hpactor/actor/typed_actor.hpp
#pragma once
#include "event_based_actor.hpp"
#include "../typed_behavior.hpp"

namespace hpactor {

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

// typed_actor reference
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

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**
Run: `cd build && make test_typed_actor && ./test_typed_actor`
Expected: PASS

- [ ] **Step 6: Commit**
```bash
git add include/hpactor/actor/typed_actor.hpp include/hpactor/typed_behavior.hpp tests/test_typed_actor.cpp
git commit -m "feat: add typed_event_based_actor and typed_behavior"
```

---

## Phase G: Mailbox Integration

### G.1: ActorMailbox

**Files:**
- Modify: `include/hpactor/mailbox.hpp` (add ActorMailbox)
- Create: `tests/test_actor_mailbox.cpp`

- [ ] **Step 1: Write failing test for ActorMailbox**

```cpp
// tests/test_actor_mailbox.cpp
#include <hpactor/mailbox.hpp>
#include <hpactor/message.hpp>
#include <cassert>

void test_actor_mailbox_interface() {
    hpactor::ActorMailbox<int> mailbox;
    assert(mailbox.empty());
    assert(mailbox.size() == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**
Expected: FAIL - ActorMailbox not defined

- [ ] **Step 3: Update mailbox.hpp to add ActorMailbox**

Add to `include/hpactor/mailbox.hpp`:

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

    bool pop_with_timeout(Message<T>& out, std::chrono::milliseconds timeout);

    void set_owner(ActorBase* owner);

private:
    mutable std::mutex mutex_;
    std::queue<Message<T>> queue_;
    std::condition_variable cv_;
    ActorBase* owner_ = nullptr;
};
```

- [ ] **Step 4: Run test to verify it passes**
Run: `cd build && make test_actor_mailbox && ./test_actor_mailbox`
Expected: PASS

- [ ] **Step 5: Commit**
```bash
git add include/hpactor/mailbox.hpp tests/test_actor_mailbox.cpp
git commit -m "feat: add ActorMailbox with owner association"
```

---

## File Structure Summary

After all phases, the file structure will be:

```
include/hpactor/
├── platform.hpp
├── message.hpp
├── mailbox.hpp
├── mutex_mailbox.hpp
├── types.hpp                      # NEW
├── types_fwd.hpp                  # NEW
├── behavior.hpp                   # NEW
├── typed_behavior.hpp              # NEW
├── actor_context.hpp               # NEW
├── actor_system.hpp                # NEW
├── actor/
│   ├── actor_fwd.hpp              # NEW
│   ├── abstract_actor.hpp         # NEW
│   ├── local_actor.hpp            # NEW
│   ├── event_based_actor.hpp      # NEW
│   ├── typed_actor.hpp            # NEW
│   ├── stateful_actor.hpp        # NEW
│   ├── blocking_actor.hpp         # NEW
│   └── scoped_actor.hpp           # NEW
└── ref/
    ├── actor_address.hpp          # NEW
    └── actor_ref.hpp              # NEW

src/
├── actor_ref.cpp                  # NEW
├── actor_context.cpp              # NEW
├── actor_system.cpp               # NEW
└── behavior.cpp                   # NEW

tests/
├── test_types.cpp                 # NEW
├── test_result.cpp                # NEW
├── test_actor_address.cpp         # NEW
├── test_actor_ref.cpp             # NEW
├── test_abstract_actor.cpp         # NEW
├── test_event_based_actor.cpp      # NEW
├── test_behavior.cpp              # NEW
├── test_actor_context.cpp          # NEW
├── test_actor_system.cpp           # NEW
├── test_blocking_actor.cpp        # NEW
├── test_stateful_actor.cpp         # NEW
├── test_typed_actor.cpp           # NEW
└── test_actor_mailbox.cpp         # NEW
```

---

## Implementation Phases Summary

| Phase | Components | Files | Tasks |
|-------|------------|-------|-------|
| A | Fundamental Types | types.hpp, types_fwd.hpp | B.1-B.2 |
| B | Actor Base Classes | actor/*.hpp, ref/*.hpp | B.1-B.4 |
| C | Behavior System | behavior.hpp, behavior.cpp | C.1 |
| D | ActorContext & ActorSystem | actor_context.hpp, actor_system.hpp | D.1-D.2 |
| E | Blocking & Stateful | blocking_actor.hpp, scoped_actor.hpp, stateful_actor.hpp | E.1-E.2 |
| F | Typed Actors | typed_actor.hpp, typed_behavior.hpp | F.1 |
| G | Mailbox Integration | mailbox.hpp (update) | G.1 |

---

## References

- Spec: `docs/superpowers/specs/2026-04-11-actor-design.md`
- Spec: `docs/architecture/actor/Actors-core-concept.md`
- CAF reference: `docs/architecture/actor/Actors.rst`
