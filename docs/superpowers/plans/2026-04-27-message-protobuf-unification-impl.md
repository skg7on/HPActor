# Message Protobuf Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `MessageVariant` + `Message<T>` with a unified `TypedMessage` that carries `TypeTag` + protobuf bytes + optional `shared_ptr<Message>` for local zero-copy delivery. All message passing uses protobuf for formalization, serialization, and deserialization. `DefaultSerializer` merges into `ProtoTypeRegistry`. `ProtoActor` handler dispatch moves into `EventBasedActor`.

**Architecture:** The single `TypedMessage` type becomes the universal carrier. Local delivery passes `shared_ptr<google::protobuf::Message>` via mailbox without serialize/deserialize. Remote delivery serializes the same protobuf message into `TypedMessage::payload_` and wraps in `WireFrame`.

**Tech Stack:** C++20, Protocol Buffers, CMake, Ninja

---

## File Structure

### New Files
| File | Purpose |
|------|---------|
| (none) | This is a refactor — no new public headers |

### Removed Files
| File | Reason |
|------|--------|
| `include/hpactor/actor/message.hpp` | `Message<T>` template replaced by `TypedMessage` |
| `include/hpactor/types/serialization.hpp` | `DefaultSerializer` merged into `ProtoTypeRegistry` |
| `include/hpactor/actor/proto_actor.hpp` | Dispatch moves into `EventBasedActor` |
| `src/core/serialization.cpp` | Logic moves into `ProtoTypeRegistry` methods |
| `src/actor/proto_actor.cpp` | Logic moves into `EventBasedActor` |

### Modified Files
| File | Change |
|------|--------|
| `include/hpactor/types/types.hpp` | Add `TypedMessage` with `shared_ptr<Message>`, `mpsc_next` |
| `include/hpactor/core/proto_type_registry.hpp` | Add `serialize_system()`/`deserialize_system()`, pre-register system types |
| `include/hpactor/actor/abstract_actor.hpp` | Remove C++ message structs, change `receive(TypedMessage&)` |
| `include/hpactor/behavior.hpp` | Change handler to `std::function<void(TypedMessage&)>` |
| `include/hpactor/actor/event_based_actor.hpp` | Absorb `ProtoHandler`, `on<T>()`, `on_request<ReqT,ResT>()` |
| `include/hpactor/core/mailbox.hpp` | Remove `ActorMailbox` use of `Message<T>` |
| `include/hpactor/core/actor_system.hpp` | `deliver_local` takes `TypedMessage`, mailbox type changes |
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | Template param changes from `Message<MessageVariant>` to `TypedMessage` |
| `include/hpactor/actor_context.hpp` | `send()`/`reply()` take `TypeTag` + `const Message&` |
| `include/hpactor/ref/actor_ref.hpp` | `send()` takes `TypedMessage` |
| `include/hpactor/ref/actor_proxy.hpp` | `send()` takes `TypedMessage` |
| `include/hpactor/sched/coroutine_awaiters.hpp` | Update awaiter type references |
| `include/hpactor/actor/stateful_actor.hpp` | Update `receive()` signature |
| `include/hpactor/actor/typed_behavior.hpp` | Update type references |
| `include/hpactor/actor/typed_event_based_actor.hpp` | Update type references |
| `protos/hpactor/messages.proto` | Add `type`/`incarnation` fields, add `completion_msg` |
| `src/actor/actor_context.cpp` | Implement new send/reply |
| `src/actor/actor_system.cpp` | `deliver_local` updated, mailbox creation updated |
| `src/actor/event_based_actor.cpp` | Absorb proto dispatch logic |
| `src/ref/actor_ref.cpp` | Simplified dispatch |
| `src/ref/actor_proxy.cpp` | Remove `std::visit`, use `TypedMessage` |
| `src/net/frame_protobuf.cpp` | Minor: use `messages.proto` types if needed |
| `src/actor/spawn_receiver.cpp` | Update to new API |
| `tests/**/*.cpp` | All 61 tests updated |
| `examples/*.cpp` | All 5 examples updated |
| `CMakeLists.txt` | Remove `proto_actor.cpp`, `serialization.cpp` from build |
| `tests/CMakeLists.txt` | Update test targets, remove proto_actor tests (merged) |

---

## Task 1: Redefine TypedMessage and Extend ProtoTypeRegistry

**Files:**
- Modify: `include/hpactor/types/types.hpp`
- Modify: `include/hpactor/core/proto_type_registry.hpp`
- Remove: `include/hpactor/types/serialization.hpp` (remove TypedMessage from here)
- Modify: `CMakeLists.txt` (remove serialization.cpp from build)

**Goal:** `TypedMessage` gains `shared_ptr<Message>` + `mpsc_next`. `ProtoTypeRegistry` gains `serialize_system()`/`deserialize_system()` and pre-registers system types.

- [ ] **Step 1: Redefine TypedMessage in types.hpp**

Move `TypedMessage` from `serialization.hpp` into `types.hpp` (after `TypeTag`). Add `shared_ptr<Message>` and `mpsc_next`:

```cpp
// In include/hpactor/types/types.hpp, after TypeTag enum

// Forward-declare protobuf base
namespace google { namespace protobuf { class Message; } }

class TypedMessage {
public:
    TypedMessage() = default;

    // Local send: parsed message + pre-serialized payload
    TypedMessage(TypeTag tag, std::shared_ptr<google::protobuf::Message> msg,
                 bytes serialized);

    // Remote receive: serialized payload only (parsed lazily)
    TypedMessage(TypeTag tag, bytes payload);

    TypeTag type_id() const { return tag_; }
    const bytes& payload() const { return payload_; }

    // Non-null if message available in parsed form (local fast path)
    std::shared_ptr<google::protobuf::Message> parsed() const { return parsed_; }

    // Lazy deserialize if only payload is available
    template<typename T>
    std::shared_ptr<T> as() const;

    // Intrusive MPSC link (replaces Message<T>::mpsc_next)
    std::atomic<TypedMessage*> mpsc_next{nullptr};

private:
    TypeTag tag_ = TypeTag::Invalid;
    bytes payload_;
    mutable std::shared_ptr<google::protobuf::Message> parsed_;
};
```

- [ ] **Step 2: Implement TypedMessage methods in a new .cpp file**

Create `src/types/typed_message.cpp`:

```cpp
#include <hpactor/types/types.hpp>
#include <google/protobuf/message.h>

namespace hpactor {

TypedMessage::TypedMessage(TypeTag tag, std::shared_ptr<google::protobuf::Message> msg,
                           bytes serialized)
    : tag_(tag), payload_(std::move(serialized)), parsed_(std::move(msg)) {}

TypedMessage::TypedMessage(TypeTag tag, bytes payload)
    : tag_(tag), payload_(std::move(payload)) {}

template<typename T>
std::shared_ptr<T> TypedMessage::as() const {
    if (parsed_) return std::static_pointer_cast<T>(parsed_);
    if (payload_.empty()) return nullptr;
    auto msg = std::make_shared<T>();
    if (!msg->ParseFromArray(payload_.data(), static_cast<int>(payload_.size())))
        return nullptr;
    parsed_ = msg;
    return msg;
}

// Explicit instantiations will be added as needed
} // namespace hpactor
```

- [ ] **Step 3: Extend ProtoTypeRegistry with system message support**

Add to `include/hpactor/core/proto_type_registry.hpp`:

```cpp
// Pre-register all system message types (called by ActorSystem constructor)
void register_system_types();

// System message serialization — takes TypeTag + MessageVariant (temporary bridge)
// Returns serialized bytes. Will be removed after full migration.
bytes serialize_system(TypeTag tag, const MessageVariant& msg);

// Returns deserialized MessageVariant. Will be removed after full migration.
MessageVariant deserialize_system(TypeTag tag, const bytes& data);
```

- [ ] **Step 4: Move DefaultSerializer logic into ProtoTypeRegistry**

In `src/core/proto_type_registry.cpp` (new file), implement `register_system_types()`, `serialize_system()`, `deserialize_system()` by moving the protobuf-based logic from `src/core/serialization.cpp`. Register system types:

```cpp
void ProtoTypeRegistry::register_system_types() {
    register_type<DownMessage>(TypeTag::DownMsg, "hpactor.DownMessage");
    register_type<ExitMessage>(TypeTag::ExitMsg, "hpactor.ExitMessage");
    register_type<LinkMessage>(TypeTag::LinkMsg, "hpactor.LinkMessage");
    register_type<UnlinkMessage>(TypeTag::UnlinkMsg, "hpactor.UnlinkMessage");
    register_type<SpawnRequestMessage>(TypeTag::SpawnRequestTag, "hpactor.SpawnRequestMessage");
    register_type<SpawnResponseMessage>(TypeTag::SpawnResponseTag, "hpactor.SpawnResponseMessage");
}
```

- [ ] **Step 5: Add `#include <hpactor/types/types.hpp>` include for TypedMessage**

Update any file that previously included `serialization.hpp` for `TypedMessage` to instead include `types.hpp`. Update `CMakeLists.txt` to add `src/types/typed_message.cpp` and `src/core/proto_type_registry.cpp` to build.

- [ ] **Step 6: Build verification**

Run: `cmake -S . -B build -GNinja && ninja -C build 2>&1 | tail -30`
Expected: Compiles. Some callers of removed APIs will have errors — those are cleaned up in subsequent tasks.

---

## Task 2: Remove Message<T>, Update Mailboxes

**Files:**
- Remove: `include/hpactor/actor/message.hpp`
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Modify: `include/hpactor/core/mailbox.hpp`
- Modify: `include/hpactor/core/actor_system.hpp` (mailbox types)
- Modify: `src/actor/actor_system.cpp` (mailbox creation, deliver_local)
- Modify: `include/hpactor/sched/coroutine_awaiters.hpp` (template param)
- Modify: `include/hpactor/actor/blocking_actor.hpp` (mailbox type if used)
- Modify: `include/hpactor/actor/scoped_actor.hpp` (mailbox type if used)

**Goal:** All `Message<MessageVariant>` replaced with `TypedMessage`. `Message<T>` deleted.

- [ ] **Step 1: Update MPSCActorMailbox template parameter**

Change `mailbox::MPSCActorMailbox<Message<MessageVariant>>` to `mailbox::MPSCActorMailbox<TypedMessage>` everywhere:

```cpp
// Before:
mailbox::MPSCActorMailbox<Message<MessageVariant>>* mailbox_;

// After:
mailbox::MPSCActorMailbox<TypedMessage>* mailbox_;
```

Files to update:
- `include/hpactor/actor/event_based_actor.hpp` (line 122, 140, 162)
- `include/hpactor/actor/abstract_actor.hpp` (line 155)
- `include/hpactor/core/actor_system.hpp` (`mailboxes_` map value type, `get_mailbox()` return type)
- `include/hpactor/sched/coroutine_awaiters.hpp` — `MailboxAwaiter` template param
- `src/actor/actor_system.cpp` (line 120, `mailboxes_` emplace, `get_mailbox()` return)

- [ ] **Step 2: Update deliver_local to use TypedMessage**

In `include/hpactor/core/actor_system.hpp` and `src/actor/actor_system.cpp`:

```cpp
// Before:
void deliver_local(ActorId target, MessageVariant msg);
void deliver_local(ActorId target, MessageVariant msg, uint8_t priority, int64_t deadline_ns);
// Body:
mailbox->push(Message<MessageVariant>(std::move(msg)));

// After:
void deliver_local(ActorId target, TypedMessage msg);
void deliver_local(ActorId target, TypedMessage msg, uint8_t priority, int64_t deadline_ns);
// Body:
mailbox->push(std::move(msg));
```

- [ ] **Step 3: Update coroutine awaiters**

In `include/hpactor/sched/coroutine_awaiters.hpp`, change `MailboxAwaiter<Message<MessageVariant>>` to `MailboxAwaiter<TypedMessage>`. Update `BlockingMailboxAwaiter` similarly.

- [ ] **Step 4: Delete message.hpp and remove all includes**

Remove `include/hpactor/actor/message.hpp`. Search and replace `#include <hpactor/actor/message.hpp>` with `#include <hpactor/types/types.hpp>` (which now has `TypedMessage`).

- [ ] **Step 5: Build verification**

Run: `ninja -C build 2>&1 | tail -40`
Expected: Compiles. Some errors from ActorContext/ActorRef/ActorProxy using old `MessageVariant` API — cleaned up in Task 3.

---

## Task 3: Update AbstractActor, Behavior, EventBasedActor

**Files:**
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `include/hpactor/behavior.hpp`
- Modify: `include/hpactor/actor/event_based_actor.hpp`
- Modify: `src/actor/event_based_actor.cpp`
- Remove: `include/hpactor/actor/proto_actor.hpp`
- Remove: `src/actor/proto_actor.cpp`
- Modify: `include/hpactor/actor/stateful_actor.hpp`
- Modify: `include/hpactor/actor/typed_behavior.hpp`
- Modify: `include/hpactor/actor/typed_event_based_actor.hpp`
- Modify: `CMakeLists.txt` (remove proto_actor.cpp from build)

**Goal:** `AbstractActor::receive(TypedMessage&)`, `Behavior` takes `TypedMessage&`, `EventBasedActor` absorbs `ProtoActor` handler dispatch, `ProtoActor` removed.

- [ ] **Step 1: Change AbstractActor::receive signature**

In `include/hpactor/actor/abstract_actor.hpp`:

```cpp
// Before:
virtual void receive(MessageVariant&& msg) = 0;

// After:
virtual void receive(TypedMessage& msg) = 0;
```

Remove the C++ message structs (`down_msg`, `exit_msg`, `link_msg`, `unlink_msg`, `completion_msg`, `ping_msg`, `pong_msg`, `stop_msg`, `start_msg`, `work_msg`, `result_msg`, `status_msg`) and the `SystemMessageVariant` / `MessageVariant` type aliases.

Keep `completion_msg` temporarily if it's used by I/O completion handling — mark it for removal in a follow-up I/O refactor if needed.

- [ ] **Step 2: Update Behavior**

In `include/hpactor/behavior.hpp`:

```cpp
// Before:
using handler_type = std::function<void(MessageVariant&&)>;
void operator()(MessageVariant&& msg) const;

// After:
using handler_type = std::function<void(TypedMessage&)>;
void operator()(TypedMessage& msg) const;
```

- [ ] **Step 3: Absorb ProtoActor into EventBasedActor**

Move `ProtoHandler` struct, `on<T>()`, `on_request<ReqT, ResT>()`, `on_proto_message()`, `handles()`, `register_handlers()`, `initialize_proto_handlers()` from `proto_actor.hpp` into `event_based_actor.hpp`.

Change `EventBasedActor::receive()` to dispatch by `TypeTag`:

```cpp
void EventBasedActor::receive(TypedMessage& msg) {
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    // First, try proto handler dispatch by TypeTag
    auto it = proto_handlers_.find(msg.type_id());
    if (it != proto_handlers_.end()) {
        auto deserialized = it->second.deserialize(msg.payload());
        if (deserialized) {
            bytes response = it->second.invoke(std::move(deserialized));
            // If response is non-empty and sender context exists, reply
            (void)response;
        }
        return;
    }

    // Fall through to Behavior-based handling
    if (behavior_) {
        behavior_(msg);
    }
}
```

- [ ] **Step 4: Remove ProtoActor files**

Delete `include/hpactor/actor/proto_actor.hpp` and `src/actor/proto_actor.cpp`. Remove proto_actor test targets from `tests/CMakeLists.txt`. Update `ProtoStatefulActor` to inherit from `EventBasedActor` instead.

- [ ] **Step 5: Update subclass receive signatures**

In `stateful_actor.hpp`, `typed_event_based_actor.hpp`, `blocking_actor.hpp`, `scoped_actor.hpp`: change `receive(MessageVariant&&)` → `receive(TypedMessage&)`.

- [ ] **Step 6: Build verification**

Run: `ninja -C build 2>&1 | tail -40`
Expected: Actor base classes compile. Remaining errors from ActorContext/ActorRef using old signatures — cleaned up in Task 4.

---

## Task 4: Update ActorContext, ActorRef, ActorProxy

**Files:**
- Modify: `include/hpactor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`
- Modify: `include/hpactor/ref/actor_ref.hpp`
- Modify: `src/ref/actor_ref.cpp`
- Modify: `include/hpactor/ref/actor_proxy.hpp`
- Modify: `src/ref/actor_proxy.cpp`

**Goal:** `ActorContext::send()` and `ActorRef::send()` take `TypedMessage`. `ActorProxy::send()` wraps `TypedMessage` directly — no `std::visit`.

- [ ] **Step 1: Update ActorContext::send()**

In `include/hpactor/actor_context.hpp`:

```cpp
// Primary send: takes a pre-constructed TypedMessage
void send(const ActorAddress& target, TypedMessage msg);

// Convenience: serializes a protobuf message and constructs TypedMessage
void send(const ActorAddress& target, TypeTag tag,
          const google::protobuf::Message& msg);

// Convenience template
template<typename ProtoMsgT>
void send(const ActorAddress& target, const ProtoMsgT& msg);

void reply(TypedMessage msg);
void reply(TypeTag tag, const google::protobuf::Message& msg);
```

In `src/actor/actor_context.cpp`:

```cpp
void ActorContext::send(const ActorAddress& target, TypedMessage msg) {
    if (target.is_local()) {
        auto actor_ptr = owner_.get();
        if (actor_ptr) {
            actor_ptr->system().deliver_local(target.id, std::move(msg));
        } else if (system_) {
            system_->deliver_local(target.id, std::move(msg));
        }
    } else {
        // Remote delivery via ActorProxy
        // TODO: wire up transport
    }
}

void ActorContext::send(const ActorAddress& target, TypeTag tag,
                        const google::protobuf::Message& msg) {
    // Serialize eagerly
    bytes payload = system_->proto_registry().serialize(msg);
    auto parsed = std::shared_ptr<google::protobuf::Message>(msg.New());
    parsed->CopyFrom(msg);
    send(target, TypedMessage(tag, std::move(parsed), std::move(payload)));
}
```

Remove old `send(MessageVariant)`, `send_with_priority(MessageVariant)`, `reply(MessageVariant)`, `schedule(MessageVariant)`, `send_proto()`, `reply_proto()`.

- [ ] **Step 2: Update ActorRef::send()**

In `include/hpactor/ref/actor_ref.hpp`:

```cpp
// Before:
void send(const ActorAddress& target, MessageVariant msg);

// After:
void send(const ActorAddress& target, TypedMessage msg);
```

In `src/ref/actor_ref.cpp`:

```cpp
void ActorRef::send(const ActorAddress& target, TypedMessage msg) {
    if (is_local()) {
        Actor* actor = get_actor();
        if (actor) {
            actor->get()->system().deliver_local(target.id, std::move(msg));
        }
    } else {
        ActorProxy* proxy = get_proxy();
        if (proxy) {
            proxy->send(target, std::move(msg));
        }
    }
}
```

- [ ] **Step 3: Simplify ActorProxy::send()**

In `include/hpactor/ref/actor_proxy.hpp`:

```cpp
// Before:
void send(const ActorAddress& target, MessageVariant msg);

// After:
void send(const ActorAddress& target, TypedMessage msg);
```

In `src/ref/actor_proxy.cpp` — replace the `std::visit` + `std::is_same_v` chain + `DefaultSerializer` with a direct wrap:

```cpp
void ActorProxy::send(const ActorAddress& target, TypedMessage msg) {
    net::WireFrame frame;
    frame.sender = address_;
    frame.receiver = target;
    frame.message_id = MessageId::generate().value();
    frame.type_tag = static_cast<uint32_t>(msg.type_id());
    frame.payload = msg.payload(); // Already serialized

    transport_->send(target, frame.encode());
}
```

- [ ] **Step 4: Build verification**

Run: `ninja -C build 2>&1 | tail -40`
Expected: All actor framework code compiles. Test and example errors remain — cleaned up in Tasks 5 and 6.

---

## Task 5: Remove DefaultSerializer, Clean Up Remaining References

**Files:**
- Remove: `include/hpactor/types/serialization.hpp`
- Remove: `src/core/serialization.cpp`
- Modify: `src/actor/spawn_receiver.cpp` (use ProtoTypeRegistry instead of DefaultSerializer)
- Modify: `src/actor/actor_system.cpp` (use ProtoTypeRegistry for spawn serialization)
- Modify: `CMakeLists.txt` (remove serialization.cpp from build)
- Modify: Any file that `#include`s serialization.hpp

**Goal:** `DefaultSerializer` is gone. All serialization goes through `ProtoTypeRegistry`.

- [ ] **Step 1: Update spawn_receiver.cpp**

Replace `DefaultSerializer` usage with `ProtoTypeRegistry`:

```cpp
// Before:
DefaultSerializer serializer;
bytes request_bytes = serializer.encode_spawn(TypeTag::SpawnRequestTag, mv);

// After:
bytes request_bytes = system_.proto_registry().serialize(pb_spawn_request);
```

- [ ] **Step 2: Update actor_system.cpp spawn_remote_async**

Replace `DefaultSerializer` + `SpawnMessageVariant` + manual protobuf construction:

```cpp
// Before:
DefaultSerializer serializer;
SpawnRequest req;
req.actor_type_name = actor_type;
// ... manual field setting
SpawnMessageVariant mv = req;
bytes request_bytes = serializer.encode_spawn(TypeTag::SpawnRequestTag, mv);

// After:
SpawnRequestMessage pb_req;
pb_req.set_actor_type_name(actor_type);
pb_req.set_args_type(static_cast<uint32_t>(TypeTag::User));
// ... field setting
bytes request_bytes = proto_registry_.serialize(pb_req);
```

- [ ] **Step 3: Remove serialization.hpp, serialization.cpp**

Delete the files. Remove from `CMakeLists.txt`. Grep for remaining `#include <hpactor/types/serialization.hpp>` and remove.

- [ ] **Step 4: Remove all references to MessageVariant, down_msg, exit_msg, etc.**

Run: `grep -rn "MessageVariant\|down_msg\|exit_msg\|link_msg\|unlink_msg\|ping_msg\|pong_msg\|stop_msg\|start_msg\|work_msg\|result_msg\|status_msg" --include="*.cpp" --include="*.hpp" src/ include/`

Replace all remaining usages. `completion_msg` may still be used in the I/O path — leave it as an internal-only struct if needed, or convert to protobuf.

- [ ] **Step 5: Build verification**

Run: `ninja -C build 2>&1 | tail -40`
Expected: Library compiles cleanly. Tests and examples still need updating.

---

## Task 6: Update All Tests

**Files:** All files under `tests/` that reference `MessageVariant`, `Message<T>`, `DefaultSerializer`, `down_msg`, `exit_msg`, `link_msg`, `unlink_msg`, `ping_msg`, `pong_msg`, `stop_msg`, `start_msg`, `work_msg`, `result_msg`, `status_msg`, `ProtoActor`, `proto_actor`.

**Goal:** All 61 tests compile and pass with the new API.

- [ ] **Step 1: Identify all tests needing changes**

Run:
```bash
grep -rl "MessageVariant\|DefaultSerializer\|down_msg\|exit_msg\|link_msg\|unlink_msg\|proto_actor" tests/ | sort
```

- [ ] **Step 2: Create test protobuf message types**

Create `protos/hpactor/test_messages.proto` with message types for tests:

```protobuf
syntax = "proto3";
package hpactor.test;

message TestPing { int32 sequence = 1; }
message TestPong { int32 sequence = 2; }
message TestRequest { int32 value = 1; }
message TestResponse { int32 result = 1; }
message TestWork { int32 value = 1; }
message TestResult { int32 value = 1; }
message TestStatus { string label = 1; }
```

Add to CMakeLists.txt protobuf generation.

- [ ] **Step 3: Update test files by category**

**Category A — Core tests** (test_types, test_result, test_serializer → remove serializer tests):
- `test_types`: Add tests for `TypedMessage` construction, move, `as<T>()`, `mpsc_next`.
- `test_serializer`: **Delete** or repurpose as `test_proto_registry` (already exists at `test_proto_registry`).
- `test_message`, `test_message_advanced`: Rewrite to test `TypedMessage` instead of `MessageVariant`.

**Category B — Mailbox tests** (test_mailbox_interface, test_mailbox_factory, test_mutex_mailbox, test_mailbox_stress, test_actor_mailbox, test_mpsc_actor_mailbox):
- Replace `Message<MessageVariant>` with `TypedMessage`.
- Replace message construction with `TypedMessage(tag, bytes{...})`.

**Category C — Actor tests** (test_abstract_actor, test_event_based_actor, test_blocking_actor, test_stateful_actor, test_typed_actor, test_actor_context, test_actor_system):
- Change `receive(MessageVariant&&)` overrides to `receive(TypedMessage&)`.
- Replace `make_behavior()` handlers to work with `TypedMessage&`.
- Use proto test message types for example messages.

**Category D — Actor reference tests** (test_actor_address, test_actor_proxy):
- Update `ActorProxy::send()` calls to pass `TypedMessage`.
- Update `ActorRef::send()` calls.

**Category E — Network tests** (test_connection, test_frame, test_tls_*, test_registrar*, test_tcp_transport*, test_event_loop, test_rpc_channel):
- Update any `MessageVariant` or `DefaultSerializer` usage.
- Most network tests should be unaffected (they test transport, not message types).

**Category F — Supervision tests** (test_supervision, test_one_for_one_supervisor, test_all_for_one_supervisor, test_supervisor_actor, test_self_supervising_actor):
- Update to use `TypedMessage` and protobuf test messages.

**Category G — Scheduling tests** (test_chaselev_deque, test_multi_priority_work_queue, test_hybrid_scheduler, test_edf_queue, test_a2ws, test_priority_scheduler, test_mpsc_actor_mailbox, test_coroutine_task, test_actor_state, test_mailbox_awaiter, test_coroutine_scheduling):
- Update `MailboxAwaiter` template params.
- Update scheduler tests that use message types.

**Category H — Proto/spawn tests** (test_proto_actor, test_proto_stateful_actor, test_proto_registry, test_async_actor, test_actor_type_registry, test_spawn_receiver, test_spawn_serialization, test_spawn_integration):
- `test_proto_actor`: **Delete** (merged into `test_event_based_actor`).
- `test_proto_stateful_actor`: Update to use `EventBasedActor`-based `ProtoStatefulActor`.
- `test_proto_registry`: Keep, update if API changed.
- Spawn tests: Update to use `ProtoTypeRegistry` instead of `DefaultSerializer`.

- [ ] **Step 4: Update tests/CMakeLists.txt**

Remove `test_proto_actor` target. Add new `test_typed_message` target if needed. Update all target_link_libraries to include `hpactor_proto`.

- [ ] **Step 5: Build and run tests**

Run: `ninja -C build && ctest --output-on-failure 2>&1 | tail -80`
Expected: All tests compile and pass.

---

## Task 7: Update Examples

**Files:** All 5 example files under `examples/`.

**Goal:** All examples compile and run correctly with the new API.

- [ ] **Step 1: Update example proto definitions**

Create or update proto files for each example's message types. For instance, for `01_echo_actor`, define `EchoRequest` / `EchoResponse` in a proto file.

- [ ] **Step 2: Update each example**

| Example | Changes |
|---------|---------|
| `01_echo_actor.cpp` | Use `on<EchoRequest>()` handler, `send_proto()` for sending |
| `02_counter_stateful.cpp` | Use `ProtoStatefulActor`, proto message handlers |
| `03_typed_calculator.cpp` | Use `on_request<AddRequest, AddResponse>()` |
| `04_supervision_tree.cpp` | Use proto messages for supervision lifecycle |
| `05_ping_pong.cpp` | Use proto `Ping`/`Pong` messages |

- [ ] **Step 3: Build and run examples**

Run: `cmake -S . -B build -GNinja -DENABLE_EXAMPLES=ON && ninja -C build`
Expected: All examples compile. Run each to verify correct behavior.

---

## Task 8: Final Verification and Cleanup

- [ ] **Step 1: Full clean build**

```bash
rm -rf build && cmake -S . -B build -GNinja && ninja -C build
```
Expected: Zero warnings, zero errors.

- [ ] **Step 2: Full test suite**

```bash
cd build && ctest --output-on-failure
```
Expected: All 61 tests pass (test count may change due to merge/removal of proto_actor tests).

- [ ] **Step 3: Check for dead code**

```bash
grep -rn "MessageVariant\|Message<\|DefaultSerializer\|mpsc_next\|proto_actor" --include="*.cpp" --include="*.hpp" src/ include/
```

Expected: `mpsc_next` found only on `TypedMessage`. `MessageVariant`, `DefaultSerializer`, old `proto_actor` class not found.

- [ ] **Step 4: Verify no stale includes**

```bash
grep -rn "message.hpp\|serialization.hpp\|proto_actor.hpp" --include="*.cpp" --include="*.hpp" src/ include/ tests/
```
Expected: No matches.

- [ ] **Step 5: Run with AddressSanitizer**

```bash
cmake -S . -B build_asan -GNinja -DENABLE_ASAN=ON && ninja -C build_asan && cd build_asan && ctest --output-on-failure
```
Expected: All tests pass (existing known ASAN false positives in `test_mailbox_awaiter` and `test_priority_scheduler` may still appear — document if so).

- [ ] **Step 6: Run with ThreadSanitizer**

```bash
cmake -S . -B build_tsan -GNinja -DENABLE_TSAN=ON && ninja -C build_tsan && cd build_tsan && ctest --output-on-failure
```
Expected: All tests pass.

---

## Summary

After completing all tasks:

- **Removed:** `Message<T>`, `MessageVariant`, `DefaultSerializer`, `Serializer`, C++ message structs (`down_msg`, `exit_msg`, etc.), `ProtoActor` as separate class
- **Added:** `TypedMessage` with `shared_ptr<Message>` local fast path + `mpsc_next`, `ProtoTypeRegistry::serialize_system()`/`deserialize_system()`
- **Merged:** `ProtoActor` handler dispatch → `EventBasedActor`, `DefaultSerializer` → `ProtoTypeRegistry`
- **Simplified:** `ActorProxy::send()` — no `std::visit`, direct wire frame wrapping
- **Unified:** All messages defined in `.proto`, serialized via `ProtoTypeRegistry`, dispatched by `TypeTag`
- **Local fast path:** `shared_ptr<Message>` through mailbox, zero-copy for local actors
- **Breaking change:** Wire format and API are fully migrated. No `MessageVariant` remains.
