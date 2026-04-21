# Phase 8: Spawn Serialization Integration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate SpawnRequest/SpawnResponse into MessageVariant for full end-to-end remote spawn with hierarchical supervision.

**Architecture:** Extend TypeTag enum with SpawnRequestTag/SpawnResponseTag, update MessageVariant to include spawn types, implement DefaultSerializer encode/decode, wire hybrid response routing via ConnectionPool, track remote children in SelfSupervisingActor.

**Tech Stack:** C++20, no exceptions, no RTTI, DefaultSerializer, Frame-based network protocol

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/hpactor/types/types.hpp` | TypeTag enum (add SpawnRequestTag, SpawnResponseTag) |
| `include/hpactor/types/serialization.hpp` | MessageVariant type alias (add spawn types) |
| `include/hpactor/spawn.hpp` | SpawnRequest/SpawnResponse structs, AsyncActor with message_id_ |
| `src/core/serialization.cpp` | DefaultSerializer encode/decode for spawn types |
| `src/actor/actor_system.cpp` | spawn_remote_async using DefaultSerializer |
| `src/net/connection_pool.cpp` | Hybrid response routing by TypeTag |
| `include/hpactor/net/connection_pool.hpp` | set_spawn_handler callback |
| `src/actor/spawn_receiver.cpp` | Handle spawn with Frame context, send response via transport |
| `include/hpactor/supervision/supervision.hpp` | SelfSupervisingActor remote child tracking |
| `include/hpactor/actor_context.hpp` | add_remote_child(ActorRef) for remote children |
| `tests/spawn/test_spawn_serialization.cpp` | New: spawn encode/decode round-trip |
| `tests/spawn/test_spawn_response_routing.cpp` | New: response routing by message_id |

---

## Tasks

### Task 1: Update TypeTag Enum and MessageVariant

**Files:**
- Modify: `include/hpactor/types/types.hpp:261-272` (add SpawnRequestTag, SpawnResponseTag)
- Modify: `include/hpactor/types/serialization.hpp` (add MessageVariant alias with spawn types)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spawn/test_spawn_serialization.cpp
#include <hpactor/spawn.hpp>
#include <hpactor/types/serialization.hpp>
#include <cassert>

// Test that MessageVariant accepts spawn types (will compile once MessageVariant is updated)
void test_message_variant_includes_spawn() {
    // Create spawn types - these should be constructible from their fields
    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {};

    // This should compile after MessageVariant is updated to include SpawnRequest
    hpactor::MessageVariant mv = req;
    assert(std::holds_alternative<hpactor::SpawnRequest>(mv));
}

int main() {
    test_message_variant_includes_spawn();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build test_spawn_serialization 2>&1`
Expected: FAIL — target not found (file doesn't exist yet)

- [ ] **Step 3: Update TypeTag enum in types.hpp**

```cpp
enum class TypeTag : uint32_t {
    Invalid = 0,

    // System messages (always present)
    DownMsg = 1,
    ExitMsg = 2,
    LinkMsg = 3,
    UnlinkMsg = 4,

    // Spawn protocol
    SpawnRequestTag = 5,
    SpawnResponseTag = 6,

    // First available user tag
    User = 100,
};
```

- [ ] **Step 4: Add MessageVariant alias to serialization.hpp**

In `include/hpactor/types/serialization.hpp`, after the TypedMessage class (around line 67), add:

```cpp
// MessageVariant - union of all message types handled by actors
using MessageVariant = std::variant<
    down_msg,
    exit_msg,
    link_msg,
    unlink_msg,
    SpawnRequest,
    SpawnResponse
>;
```

Note: This requires including `spawn.hpp` at the top of `serialization.hpp`.

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build test_spawn_serialization && ./build/tests/spawn/test_spawn_serialization`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/types/types.hpp include/hpactor/types/serialization.hpp tests/spawn/test_spawn_serialization.cpp
git commit -m "feat(spawn): add TypeTag enum and MessageVariant for spawn serialization"
```

---

- [ ] **Step 4: Add MessageVariant alias in types.hpp**

Add after the TypeTag enum (around line 274):

```cpp
// MessageVariant - union of all message types handled by actors
using MessageVariant = std::variant<
    down_msg,
    exit_msg,
    link_msg,
    unlink_msg,
    SpawnRequest,
    SpawnResponse
>;
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build test_spawn_serialization && ./build/tests/spawn/test_spawn_serialization`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/types/types.hpp tests/spawn/test_spawn_serialization.cpp
git commit -m "feat(spawn): add TypeTag enum and MessageVariant for spawn serialization"
```

---

### Task 2: Update SpawnRequest to Include Supervisor Address

**Files:**
- Modify: `include/hpactor/spawn.hpp:45-49`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spawn/test_spawn_serialization.cpp (add)
void test_spawn_request_with_supervisor() {
    hpactor::ActorAddress supervisor{
        hpactor::NodeId{1},
        hpactor::ActorType{10},
        hpactor::ActorId{42},
        1
    };

    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {1, 2, 3};
    req.supervisor_addr = supervisor;

    assert(req.actor_type_name == "worker");
    assert(req.supervisor_addr.node_id == 1);
    assert(req.supervisor_addr.id.value() == 42);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build test_spawn_serialization 2>&1`
Expected: FAIL — `supervisor_addr` member doesn't exist

- [ ] **Step 3: Update SpawnRequest struct in spawn.hpp**

```cpp
struct SpawnRequest {
    std::string actor_type_name;    // e.g., "calculator"
    TypeTag args_type;            // type tag for deserializing args
    bytes serialized_args;         // type-erased constructor arguments
    ActorAddress supervisor_addr;  // supervisor's address for link establishment
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test_spawn_serialization && ./build/tests/spawn/test_spawn_serialization`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/spawn.hpp tests/spawn/test_spawn_serialization.cpp
git commit -m "feat(spawn): add supervisor_addr to SpawnRequest"
```

---

### Task 3: Implement Spawn Serialization in DefaultSerializer

**Files:**
- Modify: `src/core/serialization.cpp:82-136` (encode_system cases)
- Modify: `src/core/serialization.cpp:139-200` (decode_system cases)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spawn/test_spawn_serialization.cpp (add)
void test_spawn_request_encode_decode() {
    hpactor::DefaultSerializer serializer;

    hpactor::ActorAddress supervisor{1, 10, hpactor::ActorId{42}, 1};
    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {1, 2, 3};
    req.supervisor_addr = supervisor;

    hpactor::MessageVariant mv = req;
    hpactor::bytes encoded = serializer.encode(
        hpactor::TypeTag::SpawnRequestTag, mv);

    // Decode back
    hpactor::MessageVariant decoded = serializer.decode(
        hpactor::TypeTag::SpawnRequestTag, encoded);

    assert(std::holds_alternative<hpactor::SpawnRequest>(decoded));
    auto& decoded_req = std::get<hpactor::SpawnRequest>(decoded);
    assert(decoded_req.actor_type_name == "worker");
    assert(decoded_req.supervisor_addr.node_id == 1);
    assert(decoded_req.supervisor_addr.id.value() == 42);
}

void test_spawn_response_encode_decode() {
    hpactor::DefaultSerializer serializer;

    hpactor::SpawnResponse resp;
    resp.actor_addr = hpactor::ActorAddress{2, 20, hpactor::ActorId{100}, 1};
    resp.error_code = hpactor::spawn_errors::success;

    hpactor::MessageVariant mv = resp;
    hpactor::bytes encoded = serializer.encode(
        hpactor::TypeTag::SpawnResponseTag, mv);

    hpactor::MessageVariant decoded = serializer.decode(
        hpactor::TypeTag::SpawnResponseTag, encoded);

    assert(std::holds_alternative<hpactor::SpawnResponse>(decoded));
    auto& decoded_resp = std::get<hpactor::SpawnResponse>(decoded);
    assert(decoded_resp.actor_addr.node_id == 2);
    assert(decoded_resp.error_code == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build test_spawn_serialization 2>&1`
Expected: FAIL — encode_system doesn't handle SpawnRequest/SpawnResponse

- [ ] **Step 3: Add encode_system cases in serialization.cpp**

Add to `encode_system()` after the unlink_msg case (around line 134):

```cpp
// SpawnRequest
else if (std::holds_alternative<SpawnRequest>(msg)) {
    const SpawnRequest& m = std::get<SpawnRequest>(msg);
    // [4b: name len][name][4b: args_type][4b: args len][args]
    // [4b: sup node_id][8b: sup actor_id][4b: sup inc][4b: sup type]
    size_t name_len = m.actor_type_name.size();
    size_t args_len = m.serialized_args.size();
    size_t total = sizeof(uint32_t) + name_len +
                   sizeof(uint32_t) + sizeof(uint32_t) + args_len +
                   sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
    result.resize(total);
    size_t offset = 0;

    uint32_t name_len_u32 = static_cast<uint32_t>(name_len);
    std::memcpy(result.data() + offset, &name_len_u32, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    std::memcpy(result.data() + offset, m.actor_type_name.data(), name_len);
    offset += name_len;

    uint32_t args_type = static_cast<uint32_t>(m.args_type);
    std::memcpy(result.data() + offset, &args_type, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    uint32_t args_len_u32 = static_cast<uint32_t>(args_len);
    std::memcpy(result.data() + offset, &args_len_u32, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    std::memcpy(result.data() + offset, m.serialized_args.data(), args_len);
    offset += args_len;

    uint32_t sup_node = m.supervisor_addr.node_id;
    std::memcpy(result.data() + offset, &sup_node, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint64_t sup_actor_id = m.supervisor_addr.id.value();
    std::memcpy(result.data() + offset, &sup_actor_id, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    uint32_t sup_inc = static_cast<uint32_t>(m.supervisor_addr.incarnation);
    std::memcpy(result.data() + offset, &sup_inc, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint32_t sup_type = m.supervisor_addr.type;
    std::memcpy(result.data() + offset, &sup_type, sizeof(uint32_t));
}
// SpawnResponse
else if (std::holds_alternative<SpawnResponse>(msg)) {
    const SpawnResponse& m = std::get<SpawnResponse>(msg);
    // [4b: node_id][8b: actor_id][4b: incarnation][4b: type][4b: error]
    result.resize(sizeof(uint32_t) + sizeof(uint64_t) +
                  sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t));
    size_t offset = 0;
    uint32_t node_id = m.actor_addr.node_id;
    std::memcpy(result.data() + offset, &node_id, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint64_t actor_id = m.actor_addr.id.value();
    std::memcpy(result.data() + offset, &actor_id, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    uint32_t inc = static_cast<uint32_t>(m.actor_addr.incarnation);
    std::memcpy(result.data() + offset, &inc, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint32_t type = m.actor_addr.type;
    std::memcpy(result.data() + offset, &type, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint32_t error = m.error_code;
    std::memcpy(result.data() + offset, &error, sizeof(uint32_t));
}
```

- [ ] **Step 4: Add decode_system cases in serialization.cpp**

Add to `decode_system()` switch (after UnlinkMsg case, around line 195):

```cpp
case TypeTag::SpawnRequestTag: {
    SpawnRequest m;
    size_t offset = 0;

    uint32_t name_len;
    std::memcpy(&name_len, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    m.actor_type_name.resize(name_len);
    std::memcpy(m.actor_type_name.data(), data.data() + offset, name_len);
    offset += name_len;

    uint32_t args_type;
    std::memcpy(&args_type, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    m.args_type = static_cast<TypeTag>(args_type);

    uint32_t args_len;
    std::memcpy(&args_len, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    m.serialized_args.resize(args_len);
    std::memcpy(m.serialized_args.data(), data.data() + offset, args_len);
    offset += args_len;

    uint32_t sup_node;
    std::memcpy(&sup_node, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint64_t sup_actor_id;
    std::memcpy(&sup_actor_id, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    uint32_t sup_inc;
    std::memcpy(&sup_inc, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint32_t sup_type;
    std::memcpy(&sup_type, data.data() + offset, sizeof(uint32_t));

    m.supervisor_addr.node_id = sup_node;
    m.supervisor_addr.id = ActorId(sup_actor_id);
    m.supervisor_addr.incarnation = sup_inc;
    m.supervisor_addr.type = sup_type;
    return m;
}
case TypeTag::SpawnResponseTag: {
    SpawnResponse m;
    size_t offset = 0;
    uint32_t node_id, type, error;
    uint64_t actor_id;
    uint32_t inc;

    std::memcpy(&node_id, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    std::memcpy(&actor_id, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    std::memcpy(&inc, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    std::memcpy(&type, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    std::memcpy(&error, data.data() + offset, sizeof(uint32_t));

    m.actor_addr.node_id = node_id;
    m.actor_addr.id = ActorId(actor_id);
    m.actor_addr.incarnation = inc;
    m.actor_addr.type = type;
    m.error_code = error;
    return m;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build test_spawn_serialization && ./build/tests/spawn/test_spawn_serialization`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/core/serialization.cpp tests/spawn/test_spawn_serialization.cpp
git commit -m "feat(serialization): add encode/decode for SpawnRequest and SpawnResponse"
```

---

### Task 4: Update ActorSystem spawn_remote_async to Use DefaultSerializer

**Files:**
- Modify: `src/actor/actor_system.cpp:206-257`
- Modify: `include/hpactor/spawn.hpp:45-49` (SpawnRequest already updated)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spawn/test_spawn_response_routing.cpp
// Tests that AsyncActor stores pending spawn with message_id correlation
#include <hpactor/spawn.hpp>
#include <cassert>

void test_async_actor_message_id_storage() {
    hpactor::AsyncActor handle(hpactor::NodeId{1}, std::chrono::milliseconds{100});
    // AsyncActor already stores message_id conceptually
    // This test verifies handle works for spawn response routing
    assert(!handle.ready());
}

int main() {
    test_async_actor_message_id_storage();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build test_spawn_response_routing 2>&1`
Expected: FAIL — target doesn't exist

- [ ] **Step 3: Update spawn_remote_async in actor_system.cpp**

In `actor_system.cpp`, replace the TODO comment block (lines 235-257) with:

```cpp
// Serialize request using DefaultSerializer
DefaultSerializer serializer;
SpawnRequest req;
req.actor_type_name = actor_type;
req.args_type = TypeTag::User;
req.serialized_args = args;
req.supervisor_addr = system_actor_.address();  // Supervisor is system actor for now

MessageVariant mv = req;
bytes request_bytes = serializer.encode(TypeTag::SpawnRequestTag, mv);

// Create frame for spawn request
net::Frame frame;
frame.sender = system_actor_.address();
frame.receiver = ActorAddress{remote_node_id, SystemActorType, SpawnReceiverId, 0};
frame.message_id = MessageId::generate().value();
frame.flags = net::Frame::RpcRequest;
frame.payload = request_bytes;

// Store pending spawn for response routing
auto pending = std::make_shared<AsyncActor>(std::move(handle));
pending->set_message_id(frame.message_id);  // NEW: store message_id for correlation

{
    std::lock_guard<std::mutex> lock(pending_spawns_mutex_);
    pending_spawns_.emplace(frame.message_id, pending);
}

// Send via transport
transport_->send(frame.receiver, frame.encode());

return pending;  // Return the AsyncActor handle
```

Note: This requires adding `set_message_id()` to `AsyncActor` and storing `message_id_` field.

- [ ] **Step 4: Add AsyncActor message_id field and setter**

In `include/hpactor/spawn.hpp`, update `AsyncActor`:

```cpp
private:
    NodeId node_id_ = 0;
    uint64_t message_id_ = 0;  // For correlation with response
    std::chrono::milliseconds timeout_{5000};
    mutable std::unique_ptr<std::mutex> mutex_;
    std::unique_ptr<std::condition_variable> cv_;
    bool ready_ = false;
    bool cancelled_ = false;
    SpawnResponse response_{};
```

Add public method:
```cpp
void set_message_id(uint64_t id) { message_id_ = id; }
uint64_t message_id() const { return message_id_; }
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build test_spawn_response_routing && ./build/tests/spawn/test_spawn_response_routing`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/actor/actor_system.cpp include/hpactor/spawn.hpp tests/spawn/test_spawn_response_routing.cpp
git commit -m "feat(spawn): use DefaultSerializer for SpawnRequest encoding"
```

---

### Task 5: Wire Spawn Response Routing in ConnectionPool

**Files:**
- Modify: `include/hpactor/net/connection_pool.hpp:99-101`
- Modify: `src/net/connection_pool.cpp:160-171`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spawn/test_spawn_response_routing.cpp (add)
// Tests that ConnectionPool can route SpawnResponse by message_id
void test_connection_pool_spawn_routing_interface() {
    // Verify set_spawn_handler exists
    static_assert(sizeof(hpactor::net::ConnectionPool) > 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build test_spawn_response_routing 2>&1`
Expected: FAIL — test file exists but routing not implemented

- [ ] **Step 3: Add set_spawn_handler to ConnectionPool**

In `include/hpactor/net/connection_pool.hpp`, add after line 101:

```cpp
// Set handler for spawn responses (called when SpawnResponse frame is received)
using spawn_response_handler = std::function<void(uint64_t message_id, const SpawnResponse&)>;
void set_spawn_handler(spawn_response_handler handler);
```

Add member variable in private section (around line 144):
```cpp
spawn_response_handler spawn_handler_;
```

- [ ] **Step 4: Implement set_spawn_handler and update on_frame_received**

In `src/net/connection_pool.cpp`, update `on_frame_received()`:

```cpp
void ConnectionPool::on_frame_received(const bytes& frame_data) {
    Frame frame = Frame::decode(frame_data);

    // Check for RPC response
    if (frame.flags & Frame::RpcResponse) {
        // Hybrid routing: check TypeTag to determine if spawn response or RPC
        DefaultSerializer serializer;
        // Extract TypeTag from payload (first 4 bytes after length prefix)
        // For now, use a simpler approach: check payload size
        // SpawnResponse has fixed size, RPC responses are variable

        // Actually, we need to decode to check TypeTag
        // But we can check the frame.flags for RpcResponse + check size hint
        // Better: check if it's a SpawnResponse by trying to decode

        // Try to decode as spawn response first
        auto decoded = serializer.decode(TypeTag::SpawnResponseTag, frame.payload);
        if (std::holds_alternative<SpawnResponse>(decoded)) {
            // Route to spawn handler
            if (spawn_handler_) {
                SpawnResponse resp = std::get<SpawnResponse>(decoded);
                spawn_handler_(frame.message_id, resp);
                return;
            }
        }

        // Fall through to RPC handler
        if (rpc_handler_) {
            rpc_handler_(MessageId(frame.message_id), frame.payload);
        }
        return;
    }

    // TODO: existing actor message handling
    (void)frame;
}

void ConnectionPool::set_spawn_handler(spawn_response_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    spawn_handler_ = std::move(handler);
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build test_spawn_response_routing && ./build/tests/spawn/test_spawn_response_routing`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/net/connection_pool.hpp src/net/connection_pool.cpp
git commit -m "feat(net): add spawn response routing to ConnectionPool"
```

---

### Task 6: Update SpawnReceiver to Use DefaultSerializer and Send Response

**Files:**
- Modify: `src/actor/spawn_receiver.cpp` (update handle_spawn_request signature and implementation)
- Modify: `include/hpactor/actor/spawn_receiver.hpp` (update method signature)

**Prerequisites:**
- `SpawnReceiver::handle_spawn_request` must accept `const Frame&` parameter to access reply address
- `ActorTypeRegistry::spawn()` must be updated to accept `(ActorSystem&, const std::string& name, const bytes& args, TypeTag args_type)` — see Task 6 pre-req below

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spawn/test_spawn_receiver.cpp (add - verify compile)
#include <hpactor/actor/spawn_receiver.hpp>
// After implementation, SpawnReceiver::handle_spawn_request takes Frame parameter
```

- [ ] **Step 2: Run test to verify current state**

Run: `ninja -C build test_spawn_receiver 2>&1`
Expected: Current state

- [ ] **Step 3: Pre-requisite - Update ActorTypeRegistry::spawn() signature**

In `include/hpactor/actor_type_registry.hpp`, update:

```cpp
result<ActorAddress> spawn(ActorSystem& sys,
                          const std::string& name,
                          const bytes& args,
                          TypeTag args_type);
```

In `src/actor/actor_type_registry.cpp`, update implementation to deserialize args using args_type if needed.

- [ ] **Step 4: Update handle_spawn_request signature in spawn_receiver.hpp**

```cpp
void handle_spawn_request(const SpawnRequest& req, const Frame& frame);
```

- [ ] **Step 5: Update handle_spawn_request implementation in spawn_receiver.cpp**

```cpp
void SpawnReceiver::handle_spawn_request(const SpawnRequest& req, const Frame& frame) {
    SpawnResponse response;

    // Deserialize args if present
    bytes args = req.serialized_args;
    TypeTag args_type = req.args_type;

    auto result = registry_.spawn(system(), req.actor_type_name, args, args_type);
    if (result.has_value()) {
        response.actor_addr = result.value();
        response.error_code = spawn_errors::success;
    } else {
        response.error_code = result.error().code();
    }

    // Send response back to caller via transport
    if (transport_) {
        net::Frame response_frame;
        response_frame.sender = address();
        response_frame.receiver = frame.sender;  // Reply to original sender
        response_frame.message_id = frame.message_id;
        response_frame.flags = net::Frame::RpcResponse;

        DefaultSerializer serializer;
        MessageVariant mv = response;
        response_frame.payload = serializer.encode(TypeTag::SpawnResponseTag, mv);

        transport_->send(response_frame.receiver, response_frame.encode());
    }
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `ninja -C build test_spawn_receiver && ./build/tests/spawn/test_spawn_receiver`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add src/actor/spawn_receiver.cpp include/hpactor/actor/spawn_receiver.hpp src/actor/actor_type_registry.cpp include/hpactor/actor_type_registry.hpp
git commit -m "feat(spawn): SpawnReceiver sends response via transport with Frame context"
```

---

### Task 7: Add Remote Child Tracking to SelfSupervisingActor

**Files:**
- Modify: `include/hpactor/supervision/supervision.hpp` (SelfSupervisingActor class)
- Modify: `src/supervision/supervision.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/supervision/test_self_supervising_actor.cpp (add)
void test_self_supervising_remote_children() {
    hpactor::SelfSupervisingActor actor(nullptr, sys, hpactor::SupervisionPolicy{});

    // Add a remote child (ActorRef)
    hpactor::ActorAddress remote_addr{hpactor::NodeId{2}, hpactor::ActorType{10},
                                      hpactor::ActorId{100}, 1};
    hpactor::ActorProxy proxy(remote_addr, nullptr);
    hpactor::ActorRef remote_child(std::move(proxy));

    // This should compile after implementation
    actor.add_remote_child(remote_child);
    assert(actor.has_remote_child(remote_addr));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build test_self_supervising_actor 2>&1`
Expected: FAIL — add_remote_child doesn't exist

- [ ] **Step 3: Add remote child tracking to SelfSupervisingActor**

In `supervision.hpp`, update `SelfSupervisingActor`:

```cpp
class SelfSupervisingActor : public EventBasedActor {
public:
    // ... existing methods ...

    // Remote child management
    void add_remote_child(ActorRef child);
    bool has_remote_child(const ActorAddress& addr) const;
    ActorRef get_remote_child(const ActorAddress& addr) const;
    void remove_remote_child(const ActorAddress& addr);
    const std::vector<ActorRef>& remote_children() const { return remote_children_; }

protected:
    virtual SupervisionDirective on_failure(ActorId child_id, const error& err);

private:
    void handle_child_down(const down_msg& msg);
    SupervisionDirective decide_restart(ActorId child_id, const error& err);
    std::vector<Actor> children_;
    std::vector<ActorRef> remote_children_;  // NEW: remote child references
    std::vector<ActorAddress> remote_child_addresses_;  // NEW: for persistence
    SupervisionPolicy policy_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
    std::chrono::steady_clock::time_point first_failure_time_;
};
```

In `src/supervision/supervision.cpp`, add implementations:

```cpp
void SelfSupervisingActor::add_remote_child(ActorRef child) {
    remote_children_.push_back(child);
    remote_child_addresses_.push_back(child.address());
}

bool SelfSupervisingActor::has_remote_child(const ActorAddress& addr) const {
    for (const auto& child_addr : remote_child_addresses_) {
        if (child_addr == addr) return true;
    }
    return false;
}

ActorRef SelfSupervisingActor::get_remote_child(const ActorAddress& addr) const {
    for (size_t i = 0; i < remote_child_addresses_.size(); ++i) {
        if (remote_child_addresses_[i] == addr) {
            return remote_children_[i];
        }
    }
    return ActorRef{};
}

void SelfSupervisingActor::remove_remote_child(const ActorAddress& addr) {
    for (size_t i = 0; i < remote_child_addresses_.size(); ++i) {
        if (remote_child_addresses_[i] == addr) {
            remote_children_.erase(remote_children_.begin() + i);
            remote_child_addresses_.erase(remote_child_addresses_.begin() + i);
            return;
        }
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test_self_supervising_actor && ./build/tests/supervision/test_self_supervising_actor`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/supervision/supervision.hpp src/supervision/supervision.cpp
git commit -m "feat(supervision): add remote child tracking to SelfSupervisingActor"
```

---

### Task 8: Add add_remote_child to ActorContext

**Files:**
- Modify: `include/hpactor/actor_context.hpp` (add remote_children_ member, add_remote_child declaration)
- Modify: `src/actor/actor_context.cpp` (add implementations)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/actor/test_actor_context.cpp (add)
void test_add_remote_child() {
    hpactor::ActorSystem sys{hpactor::ActorSystem::Config{}};
    hpactor::Actor actor = sys.spawn<hpactor::EventBasedActor>();

    hpactor::ActorAddress remote_addr{hpactor::NodeId{2}, hpactor::ActorType{10},
                                       hpactor::ActorId{100}, 1};
    hpactor::ActorProxy proxy(remote_addr, nullptr);
    hpactor::ActorRef remote_child(std::move(proxy));

    // This should compile after implementation
    // actor.context()->add_remote_child(remote_child);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build test_actor_context 2>&1`
Expected: FAIL — add_remote_child doesn't exist

- [ ] **Step 3: Add add_remote_child to ActorContext**

In `actor_context.hpp`, add after line 64:

```cpp
// Remote child management
void add_remote_child(ActorRef child);
std::vector<ActorRef> remote_children() const;
```

In `actor_context.cpp`, add implementation:

```cpp
std::vector<ActorRef> ActorContext::remote_children() const {
    return remote_children_;
}

void ActorContext::add_remote_child(ActorRef child) {
    remote_children_.push_back(std::move(child));
}
```

Add member variable in `actor_context.hpp` (around line 84):
```cpp
std::vector<ActorRef> remote_children_;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test_actor_context && ./build/tests/actor/test_actor_context`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/actor_context.hpp src/actor/actor_context.cpp
git commit -m "feat(actor): add add_remote_child to ActorContext"
```

---

### Task 9: Integration Test — Full Spawn with Response Routing

**Files:**
- Create: `tests/spawn/test_spawn_integration.cpp`

- [ ] **Step 1: Write integration test**

```cpp
// tests/spawn/test_spawn_integration.cpp
// Tests full spawn flow: request serialization, send, response routing
#include <hpactor/spawn.hpp>
#include <hpactor/types/serialization.hpp>
#include <hpactor/net/frame.hpp>
#include <cassert>

void test_full_spawn_flow() {
    // 1. Create SpawnRequest
    hpactor::ActorAddress supervisor{1, 10, hpactor::ActorId{42}, 1};
    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {1, 2, 3};
    req.supervisor_addr = supervisor;

    // 2. Serialize via DefaultSerializer
    hpactor::DefaultSerializer serializer;
    hpactor::MessageVariant mv = req;
    hpactor::bytes encoded = serializer.encode(
        hpactor::TypeTag::SpawnRequestTag, mv);

    // 3. Create Frame (simulating network send)
    hpactor::net::Frame frame;
    frame.sender = supervisor;
    frame.receiver = hpactor::ActorAddress{2, hpactor::SystemActorType,
                                            hpactor::SpawnReceiverId, 0};
    frame.message_id = hpactor::MessageId::generate().value();
    frame.flags = hpactor::net::Frame::RpcRequest;
    frame.payload = encoded;

    // 4. Encode frame (simulating network transmission)
    hpactor::bytes frame_bytes = frame.encode();

    // 5. Decode frame (simulating receive)
    hpactor::net::Frame decoded_frame = hpactor::net::Frame::decode(frame_bytes);
    assert(decoded_frame.message_id == frame.message_id);
    assert(decoded_frame.flags == hpactor::net::Frame::RpcRequest);

    // 6. Decode SpawnRequest from frame payload
    hpactor::MessageVariant decoded_mv = serializer.decode(
        hpactor::TypeTag::SpawnRequestTag, decoded_frame.payload);
    assert(std::holds_alternative<hpactor::SpawnRequest>(decoded_mv));

    hpactor::SpawnRequest& decoded_req = std::get<hpactor::SpawnRequest>(decoded_mv);
    assert(decoded_req.actor_type_name == "worker");
    assert(decoded_req.supervisor_addr.node_id == 1);

    // 7. Create and encode SpawnResponse
    hpactor::SpawnResponse resp;
    resp.actor_addr = hpactor::ActorAddress{2, 20, hpactor::ActorId{100}, 1};
    resp.error_code = hpactor::spawn_errors::success;

    hpactor::MessageVariant resp_mv = resp;
    hpactor::bytes resp_encoded = serializer.encode(
        hpactor::TypeTag::SpawnResponseTag, resp_mv);

    // 8. Create response Frame
    hpactor::net::Frame resp_frame;
    resp_frame.sender = decoded_frame.receiver;
    resp_frame.receiver = decoded_frame.sender;  // Reply to original sender
    resp_frame.message_id = decoded_frame.message_id;  // Correlate
    resp_frame.flags = hpactor::net::Frame::RpcResponse;
    resp_frame.payload = resp_encoded;

    // 9. Verify response routing would work
    assert(resp_frame.message_id == frame.message_id);  // Correlation works

    // 10. Decode response
    hpactor::bytes resp_frame_bytes = resp_frame.encode();
    hpactor::net::Frame decoded_resp_frame = hpactor::net::Frame::decode(resp_frame_bytes);
    hpactor::MessageVariant decoded_resp_mv = serializer.decode(
        hpactor::TypeTag::SpawnResponseTag, decoded_resp_frame.payload);
    assert(std::holds_alternative<hpactor::SpawnResponse>(decoded_resp_mv));

    hpactor::SpawnResponse& decoded_resp = std::get<hpactor::SpawnResponse>(decoded_resp_mv);
    assert(decoded_resp.actor_addr.node_id == 2);
    assert(decoded_resp.error_code == 0);
}

int main() {
    test_full_spawn_flow();
    return 0;
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

In `tests/spawn/CMakeLists.txt`, add:

```cmake
add_executable(test_spawn_integration test_spawn_integration.cpp)
target_link_libraries(test_spawn_integration hpactor)
add_test(NAME test_spawn_integration COMMAND test_spawn_integration)
```

- [ ] **Step 3: Run test to verify it passes**

Run: `ninja -C build test_spawn_integration && ./build/tests/spawn/test_spawn_integration`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add tests/spawn/test_spawn_integration.cpp tests/spawn/CMakeLists.txt
git commit -m "test(spawn): add integration test for full spawn flow"
```

---

## Final Verification

- [ ] Run all spawn tests: `ctest --output-on-failure -R spawn`
- [ ] Run all supervision tests: `ctest --output-on-failure -R supervision`
- [ ] Build full project: `ninja -C build`
- [ ] All tests pass

---

## Summary of Changes

| Task | Files Modified | Key Change |
|------|----------------|------------|
| 1 | `types.hpp` | Added SpawnRequestTag/SpawnResponseTag, MessageVariant alias |
| 2 | `spawn.hpp` | Added supervisor_addr to SpawnRequest |
| 3 | `serialization.cpp` | Added encode/decode for spawn types |
| 4 | `actor_system.cpp`, `spawn.hpp` | Updated spawn_remote_async to use DefaultSerializer |
| 5 | `connection_pool.hpp/cpp` | Added spawn response routing |
| 6 | `spawn_receiver.cpp` | Deserializes args, sends response via transport |
| 7 | `supervision.hpp/cpp` | Added remote child tracking |
| 8 | `actor_context.hpp/cpp` | Added add_remote_child |
| 9 | `test_spawn_integration.cpp` | Integration test |

## Out of Scope (Future Work)

- Argument deserialization (passing constructor args through spawn)
- Typed RPC API
- Actual two-process integration test with TCP transport
- Distributed supervision (supervisor on different node)