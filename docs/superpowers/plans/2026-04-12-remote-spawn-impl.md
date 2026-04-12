# Phase 6: Remote Actor Spawn — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement remote actor spawn enabling actors on one node to spawn actors on remote nodes via TCP request/response protocol.

**Architecture:** SpawnRequest/SpawnResponse over TCP using existing Frame structure. ActorTypeRegistry stores type factories. SpawnReceiver system actor handles requests. AsyncActor handle provides optional async result retrieval.

**Tech Stack:** C++20, no exceptions (-fno-exceptions), no RTTI (-fno-rtti), existing ActorSystem, Transport, ActorRef, ActorProxy, DefaultSerializer.

---

## File Structure

```
include/hpactor/
├── actor_system_ids.hpp       # NEW: well-known system ActorIds
├── spawn.hpp                  # NEW: AsyncActor, spawn system messages, spawn_errors
├── actor_type_registry.hpp    # NEW: ActorTypeRegistry (template in header)

src/
├── spawn.cpp                   # NEW: AsyncActor implementation
├── actor_type_registry.cpp     # NEW: ActorTypeRegistry non-template methods
├── actor/spawn_receiver.cpp    # NEW: SpawnReceiver actor

Modified:
├── include/hpactor/actor_system.hpp   # add spawn_remote, config fields
├── include/hpactor/serialization.hpp  # add SpawnRequest/SpawnResponse to MessageVariant
├── src/actor/actor_system.cpp         # implement spawn methods, spawn receiver init
├── CMakeLists.txt                      # add new sources
```

---

## Task 1: Create actor_system_ids.hpp

**Files:**
- Create: `include/hpactor/actor_system_ids.hpp`

- [ ] **Step 1: Create actor_system_ids.hpp**

```cpp
#pragma once

#include <hpactor/types.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// Well-known system actor IDs
// Reserved range: 0xFFFF0000 - 0xFFFFFFFF
// -----------------------------------------------------------------------------

constexpr ActorId SpawnReceiverId = ActorId(0xFFFF0001);  // Handles spawn requests

// System actor type (used in ActorAddress for system actors)
constexpr ActorType SystemActorType = 0xFFFF0000;

} // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/actor_system_ids.hpp
git commit -m "feat: add well-known system actor IDs for remote spawn

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 2: Create spawn.hpp with AsyncActor and message types

**Files:**
- Create: `include/hpactor/spawn.hpp`
- Test: `tests/spawn/test_async_actor.cpp`

- [ ] **Step 1: Create spawn.hpp**

```cpp
#pragma once

#include <hpactor/actor_system_ids.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace hpactor {

// -----------------------------------------------------------------------------
// Spawn-specific error codes (separate from general errors namespace)
// -----------------------------------------------------------------------------
namespace spawn_errors {
constexpr uint32_t success = 0;
constexpr uint32_t unknown_type = 1;
constexpr uint32_t deserialization_failed = 2;
constexpr uint32_t node_unreachable = 3;
constexpr uint32_t timeout = 4;
constexpr uint32_t spawn_receiver_not_running = 5;
} // namespace spawn_errors

// -----------------------------------------------------------------------------
// SpawnRequest - sent from caller to spawn receiver on remote node
// Note: This becomes part of MessageVariant via serialization.hpp
// -----------------------------------------------------------------------------
struct SpawnRequest {
    std::string actor_type_name;    // e.g., "calculator"
    TypeTag args_type;            // type tag for deserializing args
    bytes serialized_args;         // type-erased constructor arguments
};

// -----------------------------------------------------------------------------
// SpawnResponse - sent back from spawn receiver to caller
// Note: This becomes part of MessageVariant via serialization.hpp
// -----------------------------------------------------------------------------
struct SpawnResponse {
    ActorAddress actor_addr;    // new actor's address (node_id, type, id, incarnation)
    uint32_t error_code;       // spawn_errors::code
};

// -----------------------------------------------------------------------------
// AsyncActor - handle for asynchronous remote spawn
// -----------------------------------------------------------------------------
// Allows non-blocking spawn with result retrieval via get().
// WARNING: get() blocks the calling thread.
class AsyncActor {
public:
    AsyncActor() = default;

    // Construct with node_id and timeout
    AsyncActor(NodeId node_id, std::chrono::milliseconds timeout);

    // Wait for response and return result (blocks until response or timeout)
    result<ActorRef> get();

    // Check if response received (non-blocking)
    bool ready() const;

    // Cancel pending spawn
    void cancel();

    // Get associated node ID
    NodeId node_id() const { return node_id_; }

    // Set response (called by transport layer when response received)
    void set_response(SpawnResponse response);

private:
    NodeId node_id_ = 0;
    std::chrono::milliseconds timeout_{5000};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_ = false;
    bool cancelled_ = false;
    SpawnResponse response_{};
};

} // namespace hpactor
```

- [ ] **Step 2: Create tests/spawn directory**

```bash
mkdir -p tests/spawn
```

- [ ] **Step 3: Create test_async_actor.cpp**

```cpp
#include <hpactor/spawn.hpp>
#include <hpactor/actor_system_ids.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <thread>
#include <cassert>

using namespace hpactor;

void test_async_actor_default_constructor() {
    AsyncActor handle;
    assert(!handle.ready());
    assert(handle.node_id() == 0);
}

void test_async_actor_constructor() {
    AsyncActor handle(NodeId{42}, std::chrono::milliseconds{1000});
    assert(handle.node_id() == 42);
    assert(!handle.ready());
}

void test_async_actor_get_timeout() {
    AsyncActor handle(NodeId{1}, std::chrono::milliseconds{50});
    auto result = handle.get();
    assert(!result);  // should timeout
    assert(result.error().code() == errors::timeout);
}

void test_async_actor_response_set() {
    AsyncActor handle(NodeId{1}, std::chrono::milliseconds{100});

    // Simulate response received
    SpawnResponse resp;
    resp.actor_addr = ActorAddress{1, 100, ActorId{1}, 0};
    resp.error_code = spawn_errors::success;
    handle.set_response(resp);

    assert(handle.ready());
    auto result = handle.get();
    assert(result);
    assert(result.value().node_id() == 1);
}

void test_async_actor_cancel() {
    AsyncActor handle(NodeId{1}, std::chrono::milliseconds{1000});
    handle.cancel();
    assert(handle.ready());  // cancelled appears as ready
    auto result = handle.get();
    assert(!result);
}

int main() {
    test_async_actor_default_constructor();
    test_async_actor_constructor();
    test_async_actor_get_timeout();
    test_async_actor_response_set();
    test_async_actor_cancel();
    return 0;
}
```

- [ ] **Step 4: Create tests/spawn/CMakeLists.txt**

```cmake
add_executable(test_async_actor test_async_actor.cpp)
target_link_libraries(test_async_actor hpactor)
add_test(NAME test_async_actor COMMAND test_async_actor)
```

- [ ] **Step 5: Add to tests/CMakeLists.txt**

Add before supervision tests:
```cmake
# =============================================================================
# Spawn tests - remote actor spawn
# =============================================================================
add_subdirectory(spawn)
```

- [ ] **Step 6: Build to verify missing impl**

Run: `cmake -S . -B build -GNinja 2>&1 | tail -5 && ninja -C build test_async_actor 2>&1 | tail -20`
Expected: LINK error (undefined symbols for AsyncActor methods)

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/spawn.hpp tests/spawn/ tests/CMakeLists.txt
git commit -m "feat: add AsyncActor handle and spawn message types

AsyncActor provides non-blocking spawn with get() for result retrieval.
SpawnRequest and SpawnResponse structures for spawn protocol.
spawn_errors namespace for spawn-specific error codes.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 3: Create spawn.cpp with AsyncActor implementation

**Files:**
- Create: `src/spawn.cpp`
- Test: (covered by test_async_actor from Task 2)

- [ ] **Step 1: Create spawn.cpp**

```cpp
#include <hpactor/spawn.hpp>
#include <hpactor/ref/actor_proxy.hpp>

namespace hpactor {

AsyncActor::AsyncActor(NodeId node_id, std::chrono::milliseconds timeout)
    : node_id_(node_id), timeout_(timeout) {}

result<ActorRef> AsyncActor::get() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (cancelled_) {
        return result<ActorRef>::make(error(errors::unknown, "spawn cancelled"));
    }

    bool timed_out = !cv_.wait_for(lock, timeout_, [this] { return ready_; });
    if (timed_out) {
        return result<ActorRef>::make(error(errors::timeout, "spawn request timed out"));
    }

    if (response_.error_code != spawn_errors::success) {
        return result<ActorRef>::make(error(response_.error_code, "spawn failed"));
    }

    // Create ActorProxy for the remote actor using stack allocation
    ActorProxy proxy(response_.actor_addr, nullptr);
    ActorRef ref(std::move(proxy));
    return result<ActorRef>::make(std::move(ref));
}

bool AsyncActor::ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_ || cancelled_;
}

void AsyncActor::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    ready_ = true;
    cv_.notify_all();
}

NodeId AsyncActor::node_id() const {
    return node_id_;
}

void AsyncActor::set_response(SpawnResponse response) {
    std::lock_guard<std::mutex> lock(mutex_);
    response_ = response;
    ready_ = true;
    cv_.notify_all();
}

} // namespace hpactor
```

- [ ] **Step 2: Build and run test**

Run: `ninja -C build test_async_actor && ./build/tests/test_async_actor`
Expected: All tests pass

- [ ] **Step 3: Commit**

```bash
git add src/spawn.cpp
git commit -m "feat: implement AsyncActor handle

AsyncActor::get() blocks waiting for set_response() or timeout.
set_response() called by transport layer when SpawnResponse received.
cancel() marks handle as cancelled for immediate return.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 4: Create ActorTypeRegistry (template in header)

**Files:**
- Create: `include/hpactor/actor_type_registry.hpp` (template in header)
- Create: `src/actor_type_registry.cpp` (non-template methods only)
- Test: `tests/spawn/test_actor_type_registry.cpp`

- [ ] **Step 1: Create actor_type_registry.hpp (template in header)**

```cpp
#pragma once

#include <hpactor/actor_system_ids.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/serialization.hpp>
#include <hpactor/types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorTypeRegistry - registry of spawnable actor types on each node
// -----------------------------------------------------------------------------
// Stores factories for actor types that can be remotely spawned.
// Types are registered at startup with register_type<T>().
// Template methods are in header; non-template in actor_type_registry.cpp.
class ActorTypeRegistry {
public:
    ActorTypeRegistry() = default;

    // Register an actor type for remote spawning
    // Template implementation in header
    template<typename T>
    void register_type(const std::string& name) {
        ActorType id = next_type_id_++;
        types_by_name_[name] = TypeEntry{
            id,
            [](ActorSystem& sys) -> ActorAddress {
                Actor actor = sys.spawn<T>();
                return actor.address();
            }
        };
        names_by_type_[id] = name;
    }

    // Spawn a remote actor by name - returns ActorAddress
    result<ActorAddress> spawn(ActorSystem& system, const std::string& name);

    bool has(const std::string& name) const;
    ActorType type_id(const std::string& name) const;
    std::string type_name(ActorType type) const;

private:
    struct TypeEntry {
        ActorType type_id;
        std::function<ActorAddress(ActorSystem&)> factory;
    };

    std::unordered_map<std::string, TypeEntry> types_by_name_;
    std::unordered_map<ActorType, std::string> names_by_type_;
    ActorType next_type_id_ = ActorType{100};  // Start after reserved types
};

} // namespace hpactor
```

- [ ] **Step 2: Create src/actor_type_registry.cpp (non-template only)**

```cpp
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/actor_system.hpp>

namespace hpactor {

result<ActorAddress> ActorTypeRegistry::spawn(ActorSystem& system,
                                               const std::string& name) {
    auto it = types_by_name_.find(name);
    if (it == types_by_name_.end()) {
        return result<ActorAddress>::make(error(spawn_errors::unknown_type,
                                                "unknown actor type: " + name));
    }

    ActorAddress addr = it->second.factory(system);
    return result<ActorAddress>::make(addr);
}

bool ActorTypeRegistry::has(const std::string& name) const {
    return types_by_name_.find(name) != types_by_name_.end();
}

ActorType ActorTypeRegistry::type_id(const std::string& name) const {
    auto it = types_by_name_.find(name);
    if (it != types_by_name_.end()) {
        return it->second.type_id;
    }
    return ActorType{0};
}

std::string ActorTypeRegistry::type_name(ActorType type) const {
    auto it = names_by_type_.find(type);
    if (it != names_by_type_.end()) {
        return it->second;
    }
    return "";
}

} // namespace hpactor
```

- [ ] **Step 3: Create test_actor_type_registry.cpp**

```cpp
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/actor_system.hpp>
#include <hpactor/behavior.hpp>

#include <iostream>
#include <cassert>

using namespace hpactor;

// Test actor for registration - must be default constructible for spawn
class TestActor : public EventBasedActor {
public:
    TestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override {
        return {};
    }
};

void test_register_and_lookup() {
    ActorTypeRegistry registry;
    registry.register_type<TestActor>("test-actor");

    assert(registry.has("test-actor"));
    assert(!registry.has("non-existent"));
    assert(registry.type_id("test-actor") != ActorType{0});
}

void test_spawn_unknown_type() {
    ActorTypeRegistry registry;
    Config config;
    ActorSystem system{config};

    auto result = registry.spawn(system, "non-existent");
    assert(!result);
    assert(result.error().code() == spawn_errors::unknown_type);
}

void test_spawn_valid_type() {
    ActorTypeRegistry registry;
    Config config;
    ActorSystem system{config};

    registry.register_type<TestActor>("test-actor");
    auto result = registry.spawn(system, "test-actor");
    assert(result);
    assert(result.value().node_id() == LocalNodeId);
}

int main() {
    test_register_and_lookup();
    test_spawn_unknown_type();
    test_spawn_valid_type();
    std::cout << "All ActorTypeRegistry tests passed\n";
    return 0;
}
```

- [ ] **Step 4: Add to tests/spawn/CMakeLists.txt**

```cmake
add_executable(test_actor_type_registry test_actor_type_registry.cpp)
target_link_libraries(test_actor_type_registry hpactor)
add_test(NAME test_actor_type_registry COMMAND test_actor_type_registry)
```

- [ ] **Step 5: Build and run test**

Run: `ninja -C build test_actor_type_registry && ./build/tests/test_actor_type_registry`
Expected: Tests pass

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor_type_registry.hpp src/actor_type_registry.cpp tests/spawn/test_actor_type_registry.cpp
git commit -m "feat: add ActorTypeRegistry for remote spawn type management

ActorTypeRegistry stores actor type factories by name.
register_type<T>() registers a type for remote spawning.
spawn() creates an actor by name and returns its address.
Template implementation in header, non-template in .cpp.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 5: Create SpawnReceiver actor

**Files:**
- Create: `include/hpactor/actor/spawn_receiver.hpp`
- Create: `src/actor/spawn_receiver.cpp`
- Test: `tests/spawn/test_spawn_receiver.cpp`

- [ ] **Step 1: Create include/hpactor/actor/spawn_receiver.hpp**

```cpp
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/spawn.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// SpawnReceiver - system actor handling remote spawn requests
// -----------------------------------------------------------------------------
// Receives SpawnRequest messages, creates actors via ActorTypeRegistry,
// and sends SpawnResponse back to the caller via Transport.
class SpawnReceiver : public EventBasedActor {
public:
    SpawnReceiver(ActorSystem& sys, ActorTypeRegistry& registry, net::Transport* transport);

    Behavior make_behavior() override;

private:
    void handle_spawn_request(const SpawnRequest& req, uint64_t message_id);

    ActorTypeRegistry& registry_;
    net::Transport* transport_;  // non-owning
};

} // namespace hpactor
```

- [ ] **Step 2: Create src/actor/spawn_receiver.cpp**

```cpp
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor_system.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/serialization.hpp>

namespace hpactor {

SpawnReceiver::SpawnReceiver(ActorSystem& sys,
                             ActorTypeRegistry& registry,
                             net::Transport* transport)
    : EventBasedActor(nullptr, sys), registry_(registry), transport_(transport) {}

Behavior SpawnReceiver::make_behavior() {
    return {
        {[this](const SpawnRequest& req, uint64_t message_id) {
            handle_spawn_request(req, message_id);
        }}
    };
}

void SpawnReceiver::handle_spawn_request(const SpawnRequest& req, uint64_t message_id) {
    SpawnResponse response;

    auto result = registry_.spawn(system(), req.actor_type_name);
    if (result) {
        response.actor_addr = result.value();
        response.error_code = spawn_errors::success;
    } else {
        response.error_code = result.error().code();
    }

    // TODO: Send response back via transport using Frame.message_id to route
    // This requires transport to support reply routing
    (void)message_id;
}

} // namespace hpactor
```

- [ ] **Step 3: Create test_spawn_receiver.cpp**

```cpp
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/actor_system.hpp>

#include <iostream>
#include <cassert>

using namespace hpactor;

int main() {
    // Basic compilation test - SpawnReceiver requires full ActorSystem
    Config config;
    ActorSystem system{config};
    ActorTypeRegistry registry;

    // SpawnReceiver can't be easily unit tested without transport mock
    // This is more of an integration test
    std::cout << "SpawnReceiver compiled successfully\n";
    return 0;
}
```

- [ ] **Step 4: Add to tests/spawn/CMakeLists.txt**

```cmake
add_executable(test_spawn_receiver test_spawn_receiver.cpp)
target_link_libraries(test_spawn_receiver hpactor)
add_test(NAME test_spawn_receiver COMMAND test_spawn_receiver)
```

- [ ] **Step 5: Build and run test**

Run: `ninja -C build test_spawn_receiver && ./build/tests/test_spawn_receiver`
Expected: Test passes

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/spawn_receiver.hpp src/actor/spawn_receiver.cpp tests/spawn/test_spawn_receiver.cpp
git commit -m "feat: add SpawnReceiver system actor

SpawnReceiver handles remote spawn requests via ActorTypeRegistry.
Receives SpawnRequest, calls registry.spawn(), prepares SpawnResponse.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 6: Add spawn methods and config to ActorSystem

**Files:**
- Modify: `include/hpactor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Test: (verified via existing tests)

- [ ] **Step 1: Modify actor_system.hpp - add spawn config and methods**

Add to `Config` struct:
```cpp
struct Config {
    // ... existing fields ...

    // Remote spawn configuration
    std::chrono::milliseconds spawn_timeout{5000};
};
```

Add to public methods:
```cpp
// Remote actor spawning (main/non-actor context only)
result<ActorRef> spawn_remote(const std::string& node_name,
                               const std::string& actor_type,
                               const bytes& args);

AsyncActor spawn_remote_async(const std::string& node_name,
                               const std::string& actor_type,
                               const bytes& args);

// Actor type registry for remote spawning
ActorTypeRegistry& actor_type_registry() { return actor_type_registry_; }
const ActorTypeRegistry& actor_type_registry() const { return actor_type_registry_; }
```

Add to private members:
```cpp
ActorTypeRegistry actor_type_registry_;
std::unordered_map<uint64_t, AsyncActor> pending_spawns_;
std::mutex pending_spawns_mutex_;
```

- [ ] **Step 2: Modify actor_system.cpp - implement spawn methods**

Add includes:
```cpp
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/tcp_transport.hpp>
```

Add implementation:

```cpp
result<ActorRef> ActorSystem::spawn_remote(const std::string& node_name,
                                            const std::string& actor_type,
                                            const bytes& /*args*/) {
    AsyncActor handle = spawn_remote_async(node_name, actor_type, bytes{});
    return handle.get();
}

AsyncActor ActorSystem::spawn_remote_async(const std::string& node_name,
                                            const std::string& actor_type,
                                            const bytes& /*args*/) {
    AsyncActor handle(node_id_, config_.spawn_timeout);

    if (!config_.enable_network || !transport_) {
        // No network - mark as failed
        SpawnResponse resp;
        resp.error_code = spawn_errors::node_unreachable;
        handle.set_response(resp);
        return handle;
    }

    // TODO: Look up node by name via registrar
    // For now, assume node_name is a NodeId string
    NodeId remote_node_id = static_cast<NodeId>(std::stoul(node_name));

    // Create spawn request
    SpawnRequest request;
    request.actor_type_name = actor_type;
    request.args_type = TypeTag::User;
    request.serialized_args = bytes{};

    // Serialize request - for now, use simple manual encoding
    // TODO: Integrate with DefaultSerializer when SpawnRequest is MessageVariant
    bytes request_bytes;
    // [4 bytes: name length][name bytes...]

    // Create frame for spawn request
    Frame frame;
    frame.sender = system_actor_.address();
    frame.receiver = ActorAddress{remote_node_id, SystemActorType, SpawnReceiverId, 0};
    frame.message_id = MessageId::generate().value();
    frame.payload = request_bytes;

    // Store pending spawn for response routing
    {
        std::lock_guard<std::mutex> lock(pending_spawns_mutex_);
        pending_spawns_[frame.message_id] = handle;
    }

    // Send via transport
    transport_->send(frame.receiver, frame.encode());

    return handle;
}
```

- [ ] **Step 3: Build to verify compilation**

Run: `ninja -C build 2>&1 | head -50`
Expected: Compiles with warnings about pending_spawns_ and manual serialization

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat: add spawn_remote methods to ActorSystem

spawn_remote() - synchronous remote spawn (blocks)
spawn_remote_async() - asynchronous remote spawn (returns AsyncActor)
Both use ActorTypeRegistry for type lookup and Transport for messaging.
spawn_timeout moved to Config struct.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 7: Update CMakeLists.txt and run all tests

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add new source files to CMakeLists.txt**

Add to hpactor_lib sources:
```cmake
src/spawn.cpp
src/actor_type_registry.cpp
src/actor/spawn_receiver.cpp
```

- [ ] **Step 2: Build all tests**

Run: `ninja -C build 2>&1 | tail -20`
Expected: Build completes

- [ ] **Step 3: Run all spawn tests**

Run: `ctest -C build --output-on-failure -R spawn`
Expected: All spawn tests pass (test_async_actor, test_actor_type_registry, test_spawn_receiver)

- [ ] **Step 4: Run full test suite**

Run: `ctest -C build --output-on-failure`
Expected: All 34 tests pass (31 existing + 3 new spawn tests)

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add spawn sources to hpactor_lib

Added:
- src/spawn.cpp (AsyncActor)
- src/actor_type_registry.cpp (ActorTypeRegistry)
- src/actor/spawn_receiver.cpp (SpawnReceiver)

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 8: Add SpawnReceiver to ActorSystem initialization

**Files:**
- Modify: `src/actor/actor_system.cpp`
- Test: (verified via build)

- [ ] **Step 1: Add SpawnReceiver to ActorSystem constructor**

In the constructor where network is enabled, add:

```cpp
if (config_.enable_network) {
    // ... existing network init ...

    // Spawn the SpawnReceiver system actor using well-known ID
    // SpawnReceiverId = ActorId(0xFFFF0001) is reserved for spawn handling
    auto spawn_receiver = std::make_shared<SpawnReceiver>(
        *this, actor_type_registry_, transport_.get());
    spawn_receiver->set_address(
        ActorAddress{node_id_, SystemActorType, SpawnReceiverId, 0});

    {
        std::lock_guard<std::mutex> lock(actors_mutex_);
        actors_.emplace(SpawnReceiverId, spawn_receiver);
    }

    // Create mailbox for spawn receiver
    {
        std::lock_guard<std::mutex> lock(mailboxes_mutex_);
        mailboxes_.emplace(SpawnReceiverId,
                          std::make_unique<ActorMailbox<MessageVariant>>());
    }
}
```

Note: Using the static `SpawnReceiverId` ensures the spawn receiver is addressable at a well-known location. Remote nodes can send spawn requests to `ActorAddress{remote_node_id, SystemActorType, SpawnReceiverId, 0}`.

- [ ] **Step 2: Build to verify**

Run: `ninja -C build 2>&1 | tail -20`
Expected: Compiles

- [ ] **Step 3: Commit**

```bash
git add src/actor/actor_system.cpp
git commit -m "feat: spawn SpawnReceiver on ActorSystem network init

SpawnReceiver is created when enable_network=true and registered
in the actor registry so it can receive spawn requests.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Verification

```bash
# Build
cmake -S . -B build -GNinja && ninja -C build

# Run all tests
ctest --output-on-failure

# Expected: 34 tests pass (31 existing + 3 new)
```

---

## Summary

| Task | Description |
|------|-------------|
| 1 | actor_system_ids.hpp - well-known system ActorIds |
| 2 | spawn.hpp - AsyncActor, SpawnRequest, SpawnResponse |
| 3 | spawn.cpp - AsyncActor implementation |
| 4 | ActorTypeRegistry - template in header, non-template in .cpp |
| 5 | SpawnReceiver - system actor for spawn handling |
| 6 | ActorSystem extensions - spawn_remote methods + config |
| 7 | Build integration - add to CMake |
| 8 | SpawnReceiver integration - add to ActorSystem init |

---

## Remaining Work (Out of Scope)

The following are deferred to future iterations:
1. **Full serialization integration** - SpawnRequest/SpawnResponse as MessageVariant
2. **Transport response routing** - Transport calling AsyncActor::set_response on reply
3. **Registrar node lookup** - Translating node names to NodeId via UdpRegistrar
4. **Argument deserialization** - Passing constructor args through spawn
5. **Integration test** - Two-process remote spawn test
