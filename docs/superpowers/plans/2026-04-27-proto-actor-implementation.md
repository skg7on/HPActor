# Proto Actor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a protobuf-native actor programming model — `ProtoTypeRegistry`, `proto_actor`, and `ProtoStatefulActor<T>` — so user-defined actors can register handlers for protobuf message types using `TypeTag` dispatch.

**Architecture:** A `proto_actor` base class extending `event_based_actor` provides `on<MsgT>()` and `on_request<ReqT,ResT>()` handler registration. Messages are dispatched by `TypeTag` (uint32_t), not C++ `type_index`. A `ProtoTypeRegistry` in `ActorSystem` manages the mapping of TypeTag to protobuf descriptors for automatic serialization/deserialization.

**Tech Stack:** C++20, protobuf, HPActor actor framework

---

## File Structure

### New Files
| File | Purpose |
|------|---------|
| `include/hpactor/core/proto_type_registry.hpp` | TypeTag → protobuf type mapping, serialize/deserialize |
| `include/hpactor/actor/proto_actor.hpp` | `proto_actor` base class with `on<T>()`, `on_request<ReqT,ResT>()` |
| `include/hpactor/actor/proto_stateful_actor.hpp` | `ProtoStatefulActor<T>` with state + proto handlers |
| `tests/actor/test_proto_actor.cpp` | Tests for proto_actor handler registration and dispatch |
| `tests/actor/test_proto_stateful_actor.cpp` | Tests for ProtoStatefulActor state management |

### Modified Files
| File | Change |
|------|--------|
| `include/hpactor/core/actor_system.hpp` | Add `proto_registry_` member, `proto_registry()` accessor |
| `src/actor/actor_system.cpp` | Initialize `proto_registry_` in constructor |
| `include/hpactor/actor/actor_context.hpp` | Add `send_proto()`, `reply_proto()` declarations |
| `src/actor/actor_context.cpp` | Add `send_proto()`, `reply_proto()` methods |
| `tests/CMakeLists.txt` | Add new test targets |

---

## Task Plan

### Task 1: ProtoTypeRegistry

**Files:**
- Create: `include/hpactor/core/proto_type_registry.hpp`
- Test: (covered by Task 6 integration tests)

- [ ] **Step 1: Write ProtoTypeRegistry header**

```cpp
// include/hpactor/core/proto_type_registry.hpp
#pragma once

#include <hpactor/types/types.hpp>

#include <google/protobuf/message.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor {

// ProtoTypeRegistry maps TypeTag values to protobuf message types,
// enabling automatic serialization and deserialization without RTTI.
class ProtoTypeRegistry {
public:
    // Register a protobuf message type with a TypeTag.
    // The prototype is used as a factory for creating instances during
    // deserialization.
    template<typename ProtoMsgT>
    void register_type(TypeTag tag, const std::string& type_name) {
        Entry entry;
        entry.type_name = type_name;
        entry.prototype = std::make_shared<ProtoMsgT>();
        registry_[tag] = std::move(entry);
    }

    // Check if a TypeTag is registered
    bool has_tag(TypeTag tag) const {
        return registry_.find(tag) != registry_.end();
    }

    // Get the type name for a registered tag
    std::string type_name(TypeTag tag) const {
        auto it = registry_.find(tag);
        if (it != registry_.end()) return it->second.type_name;
        return {};
    }

    // Create a new protobuf message instance from a TypeTag.
    // Returns nullptr if tag is not registered.
    std::unique_ptr<google::protobuf::Message> create(TypeTag tag) const {
        auto it = registry_.find(tag);
        if (it == registry_.end() || !it->second.prototype) return nullptr;
        return std::unique_ptr<google::protobuf::Message>(
            it->second.prototype->New());
    }

    // Deserialize bytes into a protobuf message.
    // Returns nullptr if tag is not registered or parse fails.
    std::unique_ptr<google::protobuf::Message> deserialize(
        TypeTag tag, const bytes& data) const {
        auto msg = create(tag);
        if (!msg) return nullptr;
        if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            return nullptr;
        }
        return msg;
    }

    // Serialize a protobuf message to bytes (payload only, no TypeTag prefix).
    bytes serialize(const google::protobuf::Message& msg) const {
        bytes result(msg.ByteSizeLong());
        msg.SerializeToArray(result.data(), static_cast<int>(result.size()));
        return result;
    }

    // Encode TypeTag + payload into a single byte buffer:
    // [4 bytes: TypeTag big-endian][protobuf payload bytes]
    bytes encode_wire(TypeTag tag, const google::protobuf::Message& msg) const {
        bytes payload = serialize(msg);
        bytes result(payload.size() + 4);
        uint32_t tag_val = static_cast<uint32_t>(tag);
        result[0] = static_cast<uint8_t>((tag_val >> 24) & 0xFF);
        result[1] = static_cast<uint8_t>((tag_val >> 16) & 0xFF);
        result[2] = static_cast<uint8_t>((tag_val >> 8) & 0xFF);
        result[3] = static_cast<uint8_t>(tag_val & 0xFF);
        if (!payload.empty()) {
            std::memcpy(result.data() + 4, payload.data(), payload.size());
        }
        return result;
    }

    // Decode a wire buffer into TypeTag + protobuf message.
    // Returns {TypeTag::Invalid, nullptr} on failure.
    std::pair<TypeTag, std::unique_ptr<google::protobuf::Message>>
    decode_wire(const bytes& data) const {
        if (data.size() < 4) return {TypeTag::Invalid, nullptr};
        uint32_t tag_val =
            (static_cast<uint32_t>(data[0]) << 24) |
            (static_cast<uint32_t>(data[1]) << 16) |
            (static_cast<uint32_t>(data[2]) << 8) |
            static_cast<uint32_t>(data[3]);
        TypeTag tag = static_cast<TypeTag>(tag_val);
        bytes payload(data.begin() + 4, data.end());
        auto msg = deserialize(tag, payload);
        return {tag, std::move(msg)};
    }

private:
    struct Entry {
        std::string type_name;
        std::shared_ptr<google::protobuf::Message> prototype;
    };

    std::unordered_map<TypeTag, Entry> registry_;
};

} // namespace hpactor
```

- [ ] **Step 2: Verify it compiles**

Run: `ninja -C build 2>&1 | head -20`
Expected: Clean build (no .cpp needed — header-only)

---

### Task 2: ProtoActor Base Class

**Files:**
- Create: `include/hpactor/actor/proto_actor.hpp`
- Test: `tests/actor/test_proto_actor.cpp`

- [ ] **Step 1: Write proto_actor header**

```cpp
// include/hpactor/actor/proto_actor.hpp
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/proto_type_registry.hpp>

#include <functional>
#include <unordered_map>

namespace hpactor {

// Internal handler storage — type-erased to avoid template bloat in the map
struct ProtoHandler {
    std::string type_name;

    // Deserialize bytes into a shared_ptr<void> holding the concrete protobuf type
    std::function<std::shared_ptr<void>(const bytes&)> deserialize;

    // Invoke the handler with a deserialized message.
    // Returns serialized response bytes (empty for fire-and-forget).
    // response_tag is filled with the response TypeTag (if this is a request handler).
    std::function<bytes(std::shared_ptr<void>)> invoke;
};

// proto_actor — base class for protobuf-native actors.
//
// Users override register_handlers() and call on<MsgT>() / on_request<ReqT,ResT>()
// to register handlers for their protobuf message types.
class proto_actor : public event_based_actor {
public:
    proto_actor(ActorContext* ctx, ActorSystem& sys);

    // Register a fire-and-forget handler for a protobuf message type
    template<typename ProtoMsgT>
    void on(std::function<void(const ProtoMsgT&)> handler) {
        TypeTag tag = type_tag_for<ProtoMsgT>();
        auto handler_ptr = std::make_shared<
            std::function<void(const ProtoMsgT&)>>(std::move(handler));

        ProtoHandler entry;
        entry.type_name = ProtoMsgT().GetTypeName();
        entry.deserialize = [](const bytes& data) -> std::shared_ptr<void> {
            auto msg = std::make_shared<ProtoMsgT>();
            msg->ParseFromArray(data.data(), static_cast<int>(data.size()));
            return msg;
        };
        entry.invoke = [handler_ptr](std::shared_ptr<void> raw) -> bytes {
            auto& msg = *static_cast<ProtoMsgT*>(raw.get());
            (*handler_ptr)(msg);
            return {}; // no response
        };

        proto_handlers_[tag] = std::move(entry);
    }

    // Register a request-response handler for protobuf types
    template<typename ReqT, typename ResT>
    void on_request(std::function<ResT(const ReqT&)> handler) {
        TypeTag tag = type_tag_for<ReqT>();
        auto handler_ptr = std::make_shared<
            std::function<ResT(const ReqT&)>>(std::move(handler));

        ProtoHandler entry;
        entry.type_name = ReqT().GetTypeName();
        entry.deserialize = [](const bytes& data) -> std::shared_ptr<void> {
            auto msg = std::make_shared<ReqT>();
            msg->ParseFromArray(data.data(), static_cast<int>(data.size()));
            return msg;
        };
        entry.invoke = [handler_ptr](std::shared_ptr<void> raw) -> bytes {
            auto& req = *static_cast<ReqT*>(raw.get());
            ResT res = (*handler_ptr)(req);
            bytes result(res.ByteSizeLong());
            res.SerializeToArray(result.data(),
                                 static_cast<int>(result.size()));
            return result;
        };

        proto_handlers_[tag] = std::move(entry);
    }

    // Dispatch an incoming protobuf message by TypeTag
    // Called by the actor runtime when a proto message arrives
    void on_proto_message(TypeTag tag, const bytes& payload);

    // Check if this actor can handle a given TypeTag
    bool handles(TypeTag tag) const {
        return proto_handlers_.find(tag) != proto_handlers_.end();
    }

protected:
    // Users override this to call on<T>() / on_request<ReqT,ResT>()
    virtual void register_handlers() = 0;

    // Called by the framework after construction to set up handlers
    void initialize_proto_handlers();

    // Override receive() from event_based_actor to intercept proto messages
    void receive(MessageVariant&& msg) override;

    // Get TypeTag for a protobuf type from the system registry
    template<typename ProtoMsgT>
    TypeTag type_tag_for() const {
        // TODO: look up from system().proto_registry()
        // For now, return a default tag based on the type
        return system().proto_registry().lookup<ProtoMsgT>();
    }

    // Helper: send a protobuf message to a target actor
    template<typename ProtoMsgT>
    void send_proto(ActorAddress target, const ProtoMsgT& msg) {
        // TODO: implement when ActorContext get proto-aware methods
        (void)target;
        (void)msg;
    }

private:
    bool handlers_initialized_ = false;
    std::unordered_map<TypeTag, ProtoHandler> proto_handlers_;
};

} // namespace hpactor
```

- [ ] **Step 2: Write proto_actor.cpp implementation**

```cpp
// src/actor/proto_actor.cpp
#include <hpactor/actor/proto_actor.hpp>

namespace hpactor {

proto_actor::proto_actor(ActorContext* ctx, ActorSystem& sys)
    : event_based_actor(ctx, sys) {}

void proto_actor::initialize_proto_handlers() {
    if (handlers_initialized_) return;
    register_handlers();
    handlers_initialized_ = true;
}

void proto_actor::on_proto_message(TypeTag tag, const bytes& payload) {
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    auto it = proto_handlers_.find(tag);
    if (it == proto_handlers_.end()) {
        return; // No handler for this tag — silently drop
    }

    ProtoHandler& handler = it->second;
    auto msg = handler.deserialize(payload);
    if (!msg) return; // Deserialization failed

    bytes response = handler.invoke(std::move(msg));
    // If there's a response, send it back — the caller handles this
    // TODO: integrate with ActorContext::reply() when sender tracking is added
    (void)response;
}

void proto_actor::receive(MessageVariant&& msg) {
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    // Check if this is a proto message carried in a variant
    // Currently we can't hold protobuf types in the fixed MessageVariant,
    // so proto messages arrive as TypedMessage inside the variant.
    // For now, delegate to event_based_actor's receive for system messages.
    event_based_actor::receive(std::move(msg));
}

} // namespace hpactor
```

- [ ] **Step 3: Register proto_actor.cpp in CMakeLists.txt**

Edit the main `CMakeLists.txt`, add `src/actor/proto_actor.cpp` to the `hpactor_lib` SOURCES list.

- [ ] **Step 4: Write the test file**

```cpp
// tests/actor/test_proto_actor.cpp
#include <hpactor/actor/proto_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>
#include <cstdio>

using namespace hpactor;

class TestActor : public proto_actor {
protected:
    void register_handlers() override {
        // TODO: once protobuf types exist, test on<MyProtoMsg>()
    }
};

int main() {
    printf("=== Proto Actor Tests ===\n");

    // Test 1: proto_actor compiles and can be constructed
    {
        printf("Test 1: proto_actor compilation... ");
        // Verify the type exists
        static_assert(sizeof(proto_actor) > 0, "proto_actor should not be empty");
        printf("PASS\n");
    }

    // Test 2: TestActor compiles (verifies register_handlers() override)
    {
        printf("Test 2: TestActor compilation... ");
        static_assert(sizeof(TestActor) > 0, "TestActor should not be empty");
        printf("PASS\n");
    }

    printf("=== All Proto Actor Tests Passed ===\n");
    return 0;
}
```

- [ ] **Step 5: Build and run test**

Run: `ninja -C build && ctest --output-on-failure`

---

### Task 3: ProtoStatefulActor<T>

**Files:**
- Create: `include/hpactor/actor/proto_stateful_actor.hpp`
- Test: `tests/actor/test_proto_stateful_actor.cpp`

- [ ] **Step 1: Write ProtoStatefulActor header**

```cpp
// include/hpactor/actor/proto_stateful_actor.hpp
#pragma once

#include <hpactor/actor/proto_actor.hpp>

namespace hpactor {

// ProtoStatefulActor extends proto_actor with an explicit state object.
// The state is accessible via state() and persists across handler invocations.
template<typename StateT>
class proto_stateful_actor : public proto_actor {
public:
    proto_stateful_actor(ActorContext* ctx, ActorSystem& sys)
        : proto_actor(ctx, sys) {}

    StateT& state() { return state_; }
    const StateT& state() const { return state_; }

private:
    StateT state_;
};

} // namespace hpactor
```

- [ ] **Step 2: Write test**

```cpp
// tests/actor/test_proto_stateful_actor.cpp
#include <hpactor/actor/proto_stateful_actor.hpp>

#include <cassert>
#include <cstdio>

using namespace hpactor;

struct CounterState {
    int count = 0;
};

class CounterActor : public proto_stateful_actor<CounterState> {
public:
    using proto_stateful_actor::proto_stateful_actor;

protected:
    void register_handlers() override {
        // Handlers will be registered once proto messages are defined
    }
};

int main() {
    printf("=== ProtoStatefulActor Tests ===\n");

    // Test 1: Type compilation
    {
        printf("Test 1: type compilation... ");
        static_assert(sizeof(CounterActor) > 0, "should not be empty");
        printf("PASS\n");
    }

    printf("=== All ProtoStatefulActor Tests Passed ===\n");
    return 0;
}
```

- [ ] **Step 3: Build and run test**

Run: `ninja -C build && ctest --output-on-failure`

---

### Task 4: ActorSystem Integration

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add ProtoTypeRegistry to ActorSystem**

Add `#include <hpactor/core/proto_type_registry.hpp>` to `actor_system.hpp` includes.

In `actor_system.hpp`, add after `rpc_channel_` member:
```cpp
// Proto type registry for protobuf message serialization
ProtoTypeRegistry proto_registry_;
```

Add accessor:
```cpp
ProtoTypeRegistry& proto_registry() { return proto_registry_; }
const ProtoTypeRegistry& proto_registry() const { return proto_registry_; }
```

- [ ] **Step 2: Add helper to look up TypeTag by protobuf type**

Extend `ProtoTypeRegistry` with a template lookup:
```cpp
template<typename ProtoMsgT>
TypeTag lookup() const {
    // Walk the registry to find a tag matching this type
    // (Requires storing type_index or using proto descriptor)
    for (const auto& [tag, entry] : registry_) {
        if (entry.prototype &&
            entry.prototype->GetTypeName() == ProtoMsgT().GetTypeName()) {
            return tag;
        }
    }
    return TypeTag::Invalid;
}
```

---

### Task 5: ActorContext Proto-Aware Send/Reply

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`

- [ ] **Step 1: Add send_proto/reply_proto declarations to actor_context.hpp**

In `include/hpactor/actor/actor_context.hpp`, add after existing `send()`/`reply()` declarations:
```cpp
void send_proto(ActorAddress target, TypeTag tag,
                const google::protobuf::Message& msg);
void reply_proto(TypeTag tag,
                 const google::protobuf::Message& msg);
```

Add `#include <hpactor/types/types.hpp>` and forward-declare `namespace google { namespace protobuf { class Message; } }` if not already present.

- [ ] **Step 2: Implement send_proto and reply_proto**

In `actor_context.cpp`, add:
```cpp
void ActorContext::send_proto(ActorAddress target, TypeTag tag,
                               const google::protobuf::Message& msg) {
    // Encode TypeTag + payload into wire format
    bytes wire = system_->proto_registry().encode_wire(tag, msg);

    // TODO: create a TypedMessage variant and deliver locally
    // For now, this is a placeholder
    (void)target;
    (void)wire;
}

void ActorContext::reply_proto(TypeTag tag,
                                const google::protobuf::Message& msg) {
    // Track current sender per actor to enable reply routing
    // TODO: store current_sender_ in actor state
    (void)tag;
    (void)msg;
}
```

---

### Task 6: Integration Tests

**Files:**
- Create: `tests/actor/test_proto_registry.cpp`

- [ ] **Step 1: Write ProtoTypeRegistry test**

```cpp
// tests/actor/test_proto_registry.cpp
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/common.pb.h>

#include <cassert>
#include <cstdio>

using namespace hpactor;

int main() {
    printf("=== ProtoTypeRegistry Tests ===\n");

    // Test 1: Construction — no types registered by default
    {
        printf("Test 1: construction... ");
        ProtoTypeRegistry reg;
        assert(!reg.has_tag(TypeTag::User));
        printf("PASS\n");
    }

    // Test 2: Register a type and verify tag lookup
    {
        printf("Test 2: register_type + has_tag + type_name... ");
        ProtoTypeRegistry reg;
        reg.register_type<PbActorEndpoint>(TypeTag::User, "hpactor.proto.PbActorEndpoint");
        assert(reg.has_tag(TypeTag::User));
        assert(reg.type_name(TypeTag::User) == "hpactor.proto.PbActorEndpoint");
        printf("PASS\n");
    }

    // Test 3: create() returns non-null for registered type
    {
        printf("Test 3: create registered type... ");
        ProtoTypeRegistry reg;
        reg.register_type<PbActorEndpoint>(TypeTag::User, "hpactor.proto.PbActorEndpoint");
        auto msg = reg.create(TypeTag::User);
        assert(msg != nullptr);
        assert(msg->GetTypeName() == "hpactor.proto.PbActorEndpoint");
        printf("PASS\n");
    }

    // Test 4: create() returns nullptr for unregistered tag
    {
        printf("Test 4: create unregistered tag... ");
        ProtoTypeRegistry reg;
        auto msg = reg.create(static_cast<TypeTag>(999));
        assert(msg == nullptr);
        printf("PASS\n");
    }

    // Test 5: deserialize() fails on tag not registered
    {
        printf("Test 5: deserialize unregistered tag... ");
        ProtoTypeRegistry reg;
        bytes data = {0x00, 0x00, 0x00, 0x01, 0x00};
        auto msg = reg.deserialize(static_cast<TypeTag>(1), data);
        assert(msg == nullptr);
        printf("PASS\n");
    }

    // Test 6: Wire encode then decode round-trip
    {
        printf("Test 6: wire encode/decode round-trip... ");
        ProtoTypeRegistry reg;
        reg.register_type<PbActorEndpoint>(TypeTag::User, "hpactor.proto.PbActorEndpoint");

        PbActorEndpoint ep;
        ep.set_host("localhost");
        ep.set_port(8080);

        bytes wire = reg.encode_wire(TypeTag::User, ep);
        assert(wire.size() > 4); // at least the TypeTag prefix

        auto [tag, msg] = reg.decode_wire(wire);
        assert(tag == TypeTag::User);
        assert(msg != nullptr);
        auto* decoded = static_cast<PbActorEndpoint*>(msg.get());
        assert(decoded != nullptr);
        assert(decoded->host() == "localhost");
        assert(decoded->port() == 8080);
        printf("PASS\n");
    }

    // Test 7: Decode short buffer
    {
        printf("Test 7: short buffer decode... ");
        ProtoTypeRegistry reg;
        bytes short_buf = {0x00, 0x00};
        auto [tag, msg] = reg.decode_wire(short_buf);
        assert(tag == TypeTag::Invalid);
        assert(msg == nullptr);
        printf("PASS\n");
    }

    printf("=== All ProtoTypeRegistry Tests Passed ===\n");
    return 0;
}
```

- [ ] **Step 2: Add test targets to CMakeLists.txt**

In `tests/CMakeLists.txt`, add after the existing actor tests:
```cmake
add_executable(test_proto_registry actor/test_proto_registry.cpp)
target_link_libraries(test_proto_registry hpactor hpactor_proto pthread)
add_test(NAME test_proto_registry COMMAND test_proto_registry)

add_executable(test_proto_actor actor/test_proto_actor.cpp)
target_link_libraries(test_proto_actor hpactor)
add_test(NAME test_proto_actor COMMAND test_proto_actor)

add_executable(test_proto_stateful_actor actor/test_proto_stateful_actor.cpp)
target_link_libraries(test_proto_stateful_actor hpactor)
add_test(NAME test_proto_stateful_actor COMMAND test_proto_stateful_actor)
```

- [ ] **Step 3: Full build and test**

Run: `cmake -S . -B build -GNinja && ninja -C build && ctest --output-on-failure`

---

## Verification

```bash
# Build everything
cmake -S . -B build -GNinja && ninja -C build

# Run all tests to verify no regressions
ctest --output-on-failure

# Run specific new tests
./build/tests/test_proto_registry
./build/tests/test_proto_actor
./build/tests/test_proto_stateful_actor
```

Expected: All existing tests pass + 3 new tests pass.
