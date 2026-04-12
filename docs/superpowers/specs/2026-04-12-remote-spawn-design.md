# Phase 6: Remote Actor Spawn — Design Specification

**Date:** 2026-04-12
**Status:** Approved (revised after review)
**Dependencies:** Phase 5 (Service Discovery)

## Overview

Remote actor spawn allows an actor on one node to create and manage actors on remote nodes. The system uses a request/response protocol over TCP — the caller sends a spawn request, the remote node creates the actor and replies with its address. The caller receives an `ActorProxy` that transparently routes messages over the network.

## Architecture

### High-Level Flow

```
Caller (Node A)                    Remote Node (Node B)
     |                                     |
     | --- TCP: SpawnRequest -------------> |
     |     actor_type="calculator"         |
     |     args=[serialized]               |
     |     args_type=TypeTag::User        |
     |     Frame.sender = caller_addr     |
     |                                     | --- deserialize args using args_type
     |                                     | --- lookup "calculator" type
     |                                     | --- ActorSystem::spawn<T>(args...)
     |                                     |
     | <-- TCP: SpawnResponse ------------- |
     |     actor_addr={B, type, id, inc}   |
     |     error=0                         |
     |                                     |
[ActorProxy created for actor_addr by ActorSystem]
```

### Components

1. **Spawn Protocol** — `SpawnRequest` / `SpawnResponse` messages over TCP
2. **ActorTypeRegistry** — registry of spawnable actor types on each node
3. **SpawnReceiver actor** — system actor handling spawn requests
4. **ActorSystem extensions** — `spawn_remote()` and `spawn_remote_async()`
5. **AsyncActor handle** — for non-blocking spawn with result retrieval

## Spawn Protocol

### Well-Known SpawnReceiver ActorId

System actors use reserved ActorId values in a special range:

```cpp
// In actor_system_ids.hpp
constexpr ActorId SpawnReceiverId = ActorId(0xFFFF0001);  // Reserved system range
```

### SpawnRequest Message

```cpp
struct SpawnRequest {
    std::string actor_type_name;    // e.g., "calculator"
    TypeTag args_type;             // type tag for deserializing args
    bytes serialized_args;         // type-erased constructor arguments
};
```

**Note:** The reply address is taken from `Frame.sender`, not from a payload field. This is consistent with how other messages work.

**Wire format** (serialized via DefaultSerializer):
```
[4 bytes: name length][name bytes...]
[4 bytes: args_type]
[4 bytes: args length][args bytes...]
```

### SpawnResponse Message

```cpp
struct SpawnResponse {
    ActorAddress actor_addr;    // new actor's address
    uint32_t error_code;       // 0 = success, non-zero = failure
};
```

**Spawn-specific error codes** (separate from general `errors` namespace):

```cpp
namespace spawn_errors {
constexpr uint32_t success = 0;
constexpr uint32_t unknown_type = 1;
constexpr uint32_t deserialization_failed = 2;
constexpr uint32_t node_unreachable = 3;
constexpr uint32_t timeout = 4;
constexpr uint32_t spawn_receiver_not_running = 5;
}
```

### Frame Usage

Spawn messages use the existing `Frame` structure:
- `sender`: caller node + caller actor address (for reply routing)
- `receiver`: `ActorAddress{remote_node_id, ActorType::SystemActor, SpawnReceiverId, 0}`
- `message_id`: unique ID for matching response to request
- `payload`: SpawnRequest or SpawnResponse

Note: `ActorType::SystemActor` is a reserved actor type for system-level actors like SpawnReceiver.

## ActorTypeRegistry

### Interface

```cpp
class ActorTypeRegistry {
public:
    ActorTypeRegistry();

    // Register an actor type for remote spawning (new template method)
    template<typename T, typename... Args>
    void register_type(const std::string& name);

    // Spawn a remote actor by name
    result<ActorAddress> spawn(const std::string& name,
                                const bytes& args);

    bool has(const std::string& name) const;
    ActorType type_id(const std::string& name) const;
};
```

### Registration

Types are registered at startup:

```cpp
// Example - NEW template method added to ActorSystem
system.register_actor_type<CalculatorActor>("calculator");
```

The template method:
1. Generates a unique `ActorType` ID for this type
2. Stores a factory function that deserializes args and constructs the actor
3. Registers the type with the local `ActorSystem`

### Spawn Operation

1. Look up factory by name
2. Call factory with deserialized arguments
3. Return new actor's address

## SpawnReceiver Actor

A system actor running on each network-enabled node:

```cpp
class SpawnReceiver : public EventBasedActor {
public:
    SpawnReceiver(ActorSystem& sys, ActorTypeRegistry& registry);

    Behavior make_behavior() override;

private:
    void handle_spawn_request(const SpawnRequest& req, const Frame& frame);

    ActorTypeRegistry& registry_;
};
```

**Behavior:**
- Receives `SpawnRequest` messages at `SpawnReceiverId`
- Calls `registry_.spawn()` to create the actor
- Replies to `frame.sender` with `SpawnResponse`

## ActorSystem Extensions

### Configuration

```cpp
struct Config {
    // ... existing fields ...

    // Remote spawn configuration
    std::chrono::milliseconds spawn_timeout{5000};
    bool enable_remote_spawn = true;
};
```

### API

```cpp
class ActorSystem {
public:
    // Register an actor type for remote spawning (NEW template method)
    template<typename T, typename... Args>
    void register_actor_type(const std::string& name);

    // Synchronous remote spawn
    // WARNING: Must only be called from non-actor context (e.g., main thread)
    // Calling from within an actor may cause deadlock
    result<ActorRef> spawn_remote(const std::string& node_name,
                                   const std::string& actor_type,
                                   const bytes& args);

    // Asynchronous remote spawn (preferred for actor context)
    AsyncActor spawn_remote_async(const std::string& node_name,
                                  const std::string& actor_type,
                                  const bytes& args);
};
```

### Return Type: ActorRef

The return type is `ActorRef`, not `Actor`. This is because:
- `Actor` is a handle for local actors (contains `std::shared_ptr<AbstractActor>`)
- Remote actors are represented by `ActorProxy`, which is wrapped in `ActorRef`
- `ActorRef` provides location-transparent access (the caller doesn't need to know if the actor is local or remote)

### AsyncActor Handle

```cpp
class AsyncActor {
public:
    AsyncActor() = default;

    // Returns when response received or timeout
    // WARNING: Blocks the calling thread
    result<ActorRef> get();

    // Check if response received (non-blocking)
    bool ready() const;

    // Cancel pending spawn request
    void cancel();

    // Get associated node ID for this spawn
    NodeId node_id() const;
};
```

**Thread safety:** `AsyncActor::get()` blocks the calling thread. For actor contexts, prefer message-passing patterns instead.

## Error Handling

All spawn operations return `result<ActorRef>`:

```cpp
// WARNING: spawn_remote blocks - only call from main/non-actor context
auto result = system.spawn_remote("node-b", "calculator", args);
if (!result) {
    // Handle error
    log_error("Spawn failed: ", result.error().message());
    return;
}
ActorRef remote_actor = result.value();
```

For actor contexts, use the async version and handle the response via message passing.

## Usage Examples

### Node B: Register actor type

```cpp
// At startup - NEW template method
system.register_actor_type<WorkerActor>("worker");
system.register_actor_type<CalculatorActor>("calculator");
```

### Node A: Spawn synchronously (main context only)

```cpp
// Serialize arguments
DefaultSerializer serializer;
bytes args = serializer.encode(TypeTag::User, worker_args);

auto result = node_a.spawn_remote("node-b", "worker", args);
if (result) {
    ActorRef worker = result.value();
    // Send messages to remote actor
}
```

### Node A: Spawn asynchronously (for actor contexts)

```cpp
// Spawn asynchronously - handle is an AsyncActor
AsyncActor handle = node_a.spawn_remote_async("node-b", "calculator", args);

// Continue other work...

// Later, get the result (blocks until response or timeout)
auto result = handle.get();
if (result) {
    ActorRef calc = result.value();
    // Use the actor
}
```

Note: The async handle's result is retrieved by calling `get()` on the handle, not via a separate message handler. The `SpawnResponse` is routed internally by the framework.

## Implementation Tasks

1. Create `actor_system_ids.hpp` with well-known system ActorIds
2. Create `ActorTypeRegistry` class with template registration
3. Create `SpawnReceiver` event-based actor
4. Create `AsyncActor` handle class
5. Add template `register_actor_type<T>()` to `ActorSystem`
6. Add `spawn_remote()` and `spawn_remote_async()` to `ActorSystem`
7. Integrate spawn receiver into network startup
8. Add tests

## Testing

### Unit Tests
- `test_actor_type_registry.cpp` — registration, spawn, type lookup
- `test_spawn_receiver.cpp` — request handling, response sending
- `test_async_actor.cpp` — async handle timeout, cancel, ready

### Integration Tests
- `test_remote_spawn.cpp` — two-process test:
  - Node B starts with registered types
  - Node A spawns actor on Node B
  - Verify ActorAddress returned with correct node_id
  - Verify messages can be sent to remote actor

## Files to Create/Modify

### New Files
- `include/hpactor/actor_system_ids.hpp` — well-known system ActorIds
- `include/hpactor/spawn.hpp` — AsyncActor, SpawnRequest, SpawnResponse, spawn_errors
- `include/hpactor/actor_type_registry.hpp` — ActorTypeRegistry
- `src/spawn.cpp` — AsyncActor implementation
- `src/actor_type_registry.cpp` — ActorTypeRegistry implementation
- `src/actor/spawn_receiver.cpp` — SpawnReceiver implementation
- `tests/spawn/test_actor_type_registry.cpp`
- `tests/spawn/test_spawn_receiver.cpp`
- `tests/spawn/test_async_actor.cpp`
- `tests/spawn/CMakeLists.txt`

### Modified Files
- `include/hpactor/actor_system.hpp` — add spawn_remote, register_actor_type template
- `src/actor/actor_system.cpp` — implement spawn methods
- `tests/CMakeLists.txt` — add new test directory

## Out of Scope

- Actor migration (moving actors between nodes)
- Distributed supervision (supervising actors on remote nodes)
- Actor death notification across nodes (handled by existing link/monitor system)
- Load balancing or placement strategies
