# HPActor Framework Tutorial

A practical guide to using the HPActor C++ actor framework.

## Table of Contents

1. [Introduction](#introduction)
2. [Actor Types](#actor-types)
3. [Message Passing](#message-passing)
4. [Typed Actors](#typed-actors)
5. [Supervision](#supervision)
6. [Examples](#examples)

---

## Introduction

HPActor is an event-based actor framework for C++20 following the CAF (C++ Actor Framework) model. Actors are independent units of computation that communicate exclusively through message passing.

**Key characteristics:**
- Event-based actors with cooperative scheduling
- Statically typed message handling via `typed_actor<>`
- Hierarchical supervision for fault-tolerance
- Header-only library (no external dependencies)

---

## Actor Types

### EventBasedActor

The fundamental actor type for cooperative, event-driven message processing.

```cpp
#include <hpactor/actor/event_based_actor.hpp>

class MyActor : public hpactor::EventBasedActor {
  protected:
    // Define initial behavior
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::MessageVariant&& msg) {
            // Handle messages
        }};
    }

  public:
    MyActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {}
};
```

**Key methods:**
- `make_behavior()` — Override to define initial message handling
- `become(Behavior)` — Switch to a new behavior at runtime
- `become_empty()` — Drop all messages

### StatefulActor<T>

An `EventBasedActor` with explicit state management.

```cpp
#include <hpactor/actor/stateful_actor.hpp>

struct CounterState {
    int value = 0;
    int max_value = 100;
};

class CounterActor : public hpactor::StatefulActor<CounterState> {
  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::MessageVariant&& msg) {
            // Access state via state().member
        }};
    }

  public:
    CounterActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::StatefulActor<CounterState>(ctx, sys) {
        state().name = "counter";  // Initialize state
    }
};
```

**Key methods:**
- `state()` — Access mutable state reference
- `state() const` — Access immutable state reference

### BlockingActor

For actors that need blocking receive (runs in own thread).

```cpp
#include <hpactor/actor/blocking_actor.hpp>

class WorkerActor : public hpactor::BlockingActor {
  public:
    WorkerActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::BlockingActor(ctx, sys) {}

    void run() {
        // Blocking receive loop
        receive(
            [](MessageA msg) { /* handle */ },
            [](MessageB msg) { /* handle */ }
        );
    }
};
```

### ScopedActor

A `BlockingActor` for non-actor contexts (e.g., `main()`).

```cpp
#include <hpactor/actor/scoped_actor.hpp>

int main() {
    hpactor::ActorSystem system{config};
    hpactor::ScopedActor scope{system};

    // Act as actor from main thread
    scope.send(actor.address(), MyMessage{});

    // Blocking receive
    auto response = scope.receive<Response>();
}
```

---

## Message Passing

### Sending Messages

From within an actor, use `ActorContext`:

```cpp
// Send to another actor
context()->send(target_address, MyMessage{42});

// Reply to sender
context()->reply(ResponseMessage{result});

// Reply with error
context()->reply_with_error(hpactor::error{1, "failed"});
```

### Linking and Monitoring

**Linking** — death sharing (bidirectional):

```cpp
// Link to another actor
actor->link_to(other_address);

// Remove link
actor->unlink_from(other_address);

// When linked actor dies, you receive:
hpactor::down_msg{terminated_actor, reason}
```

**Monitoring** — one-way death notification:

```cpp
// Monitor another actor
context()->monitor(target_address);

// When monitored actor dies, you receive:
hpactor::down_msg{terminated_actor, reason}
```

### Message Patterns

**Request-Response:**

```cpp
// Actor A sends request
context()->send(b_address, Request{data});

// Actor B replies
context()->reply(Response{result});

// Actor A handles response
Behavior make_behavior() override {
    return hpactor::Behavior{[this](hpactor::MessageVariant&& msg) {
        std::visit([this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Response>) {
                // Handle response
            }
        }, msg);
    }};
}
```

**Fire-and-Forget:**

```cpp
context()->send(target_address, LogMessage{"info", "something happened"});
```

---

## Typed Actors

For compile-time type-safe message handling.

### Defining Typed Signatures

```cpp
#include <hpactor/actor/typed_actor.hpp>
#include <hpactor/typed_behavior.hpp>

// Define message types
struct AddMessage { int a; int b; };
struct ShutdownMessage {};

// Typed actor with signatures
using CalculatorActor = hpactor::typed_actor<
    hpactor::result<int>(AddMessage),       // returns int
    hpactor::result<void>(ShutdownMessage)  // returns void
>;
```

### Signature Format

```
hpactor::result<ReturnType>(MessageType)
```

| Return Type | Meaning |
|-------------|---------|
| `result<T>` | Handler returns `T` |
| `result<void>` | No response expected |

### Implementing Typed Actor

```cpp
class Calculator
    : public hpactor::TypedEventBasedActor<
          hpactor::result<int>(AddMessage),
          hpactor::result<void>(ShutdownMessage)> {

  protected:
    behavior_type make_behavior() override {
        behavior_type bh;

        // Register handler
        bh([](AddMessage msg) -> hpactor::result<int> {
            return hpactor::result<int>::make(msg.a + msg.b);
        });

        bh([](ShutdownMessage) -> hpactor::result<void> {
            return hpactor::result<void>::make();
        });

        return bh;
    }

  public:
    Calculator(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : TypedEventBasedActor(ctx, sys) {}
};
```

### Using Typed Actor Reference

```cpp
// Get typed reference
CalculatorActor calc = system.spawn<Calculator>();

// Type-safe message sending (compiler enforces correct types)
calc(AddMessage{10, 5});  // OK
calc(ShutdownMessage{});  // OK

// Wrong type — compiler error!
// calc("hello");  // Error: no matching function
```

### Handler Type Extraction

The `handler_type` template extracts type information:

```cpp
using AddHandler = hpactor::handler_type<hpactor::result<int>(AddMessage)>;
using AddResult = typename AddHandler::result;    // int
using AddMsg = typename AddHandler::message;      // AddMessage
```

---

## Supervision

Fault-tolerance through hierarchical error handling.

### SupervisionPolicy

```cpp
hpactor::SupervisionPolicy policy{
    .strategy = hpactor::SupervisionPolicy::Strategy::OneForOne,
    .max_restarts = 3,
    .restart_interval = std::chrono::seconds(5)
};
```

**Strategies:**
- `OneForOne` — Only failed child is restarted
- `AllForOne` — All children are restarted when one fails

### SupervisorActor

```cpp
#include <hpactor/supervision/one_for_one_supervisor.hpp>

class DatabaseSupervisor : public hpactor::SupervisorActor {
  public:
    DatabaseSupervisor(hpactor::ActorContext* ctx,
                       hpactor::ActorSystem& sys,
                       hpactor::SupervisionPolicy policy,
                       std::vector<hpactor::Actor> children)
        : hpactor::SupervisorActor(ctx, sys, strategy_, std::move(children)),
          strategy_(policy) {}

  private:
    hpactor::OneForOneSupervisor strategy_;
};
```

### SelfSupervisingActor

For actors that manage their own children:

```cpp
class SessionManager : public hpactor::SelfSupervisingActor {
  public:
    SessionManager(hpactor::ActorContext* ctx,
                   hpactor::ActorSystem& sys,
                   hpactor::SupervisionPolicy policy)
        : hpactor::SelfSupervisingActor(ctx, sys, policy),
          policy_(policy) {}

  protected:
    // Custom restart logic
    hpactor::SupervisionDirective on_failure(
        hpactor::ActorId child_id,
        const hpactor::error& err) override {
        std::cout << "Child " << child_id.value() << " failed: "
                  << err.message() << std::endl;
        return hpactor::SupervisionDirective::Restart;
    }

  private:
    hpactor::SupervisionPolicy policy_;
};
```

### Supervision Flow

```
1. Child encounters error
        |
        v
2. Child sends down_msg to parent
        |
        v
3. Supervisor receives down_msg
        |
        v
4. Strategy decides action (Restart/Stop/Escalate)
        |
        v
5. Execute directive
```

### SupervisionDirective

| Directive | Behavior |
|-----------|----------|
| `Restart` | Restart the failed child |
| `Stop` | Stop the child (and siblings if AllForOne) |
| `Escalate` | Pass to this actor's supervisor |

---

## Examples

See the `examples/` directory for working examples:

| Example | File | Demonstrates |
|---------|------|--------------|
| Echo Actor | `01_echo_actor.cpp` | EventBasedActor, behavior, become() |
| Counter | `02_counter_stateful.cpp` | StatefulActor<T>, state management |
| Calculator | `03_typed_calculator.cpp` | typed_actor<>, TypedBehavior |
| Supervision | `04_supervision_tree.cpp` | SupervisorActor, OneForOne, AllForOne |
| Ping-Pong | `05_ping_pong.cpp` | Actor communication, ScopedActor, linking |

### Building Examples

```bash
# Add to your CMakeLists.txt
add_subdirectory(examples)

# Or build directly
cd examples
mkdir -p build && cd build
cmake ..
make
```

---

## API Reference

### ActorSystem

```cpp
hpactor::Config config{
    .scheduler_threads = 4,
    .max_queue_depth = 1024
};
hpactor::ActorSystem system(config);

// Spawn actor (when implemented)
auto actor = system.spawn<MyActor>();

// Register/resolve by name
system.register_actor("my_actor", actor);
auto resolved = system.resolve_actor("my_actor");
```

### ActorContext

```cpp
// Spawn child actor
auto child = context()->spawn<ChildActor>();

// Send message
context()->send(target_address, message);

// Children management
context()->add_child(child);
context()->remove_child(child);
auto children = context()->children();

// Monitoring
context()->monitor(target_address);
```

### MessageVariant

The variant type for all messages (system + user-defined):

```cpp
using hpactor::MessageVariant = std::variant<
    hpactor::down_msg,      // Actor died
    hpactor::exit_msg,      // Exit message
    hpactor::link_msg,      // Link request
    hpactor::unlink_msg,    // Unlink request
    // ... user-defined types
>;
```

To add user messages, create a custom variant type:

```cpp
using MyMessageVariant = std::variant<
    MyMessageA,
    MyMessageB,
    hpactor::down_msg  // Include system messages
>;
```

---

## Common Patterns

### Actor Factory

```cpp
template <typename ActorType, typename... Args>
hpactor::Actor spawn_actor(hpactor::ActorSystem& system, Args&&... args) {
    return system.spawn<ActorType>(std::forward<Args>(args)...);
}
```

### Actor Registry

```cpp
class ActorRegistry {
  public:
    void registerActor(const std::string& name, hpactor::Actor actor) {
        registry_[name] = actor.address();
    }

    hpactor::ActorAddress resolve(const std::string& name) {
        return registry_[name];
    }

  private:
    std::unordered_map<std::string, hpactor::ActorAddress> registry_;
};
```

### Graceful Shutdown

```cpp
// Send shutdown to all children
for (auto child : context()->children()) {
    context()->send(child.address(), ShutdownMessage{});
}

// Wait for confirmation
// (In practice, use a countdown latch or similar)
```

---

## Best Practices

1. **Keep actors focused** — Single responsibility principle
2. **Don't block in message handlers** — Use event-based patterns
3. **Use typed actors for APIs** — Compile-time type checking
4. **Set supervision limits** — Prevent infinite restart loops
5. **Log actor lifecycle events** — For debugging supervision
6. **Use structured message types** — Don't use primitive types for messages

---

## Limitations

**Current status:** Runtime infrastructure (spawn, send, scheduler) is not yet fully implemented. The framework provides the type system and supervision logic, but actors cannot yet exchange messages at runtime.

The examples in this tutorial demonstrate the intended API design and serve as documentation for when the runtime is completed.
