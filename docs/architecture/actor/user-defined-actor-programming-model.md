# User-Defined Actor Programming Model — Protobuf-Based Message Handling

## 1. Executive Summary

This document specifies the programming model for user-defined actors in HPActor, focused on **protobuf-based message handling**. The current framework supports system messages (`down_msg`, `exit_msg`, etc.) via a fixed `MessageVariant` variant, but user-defined message types require a separate, extensible path.

**Key Design Decisions:**
- User messages are protobuf-generated C++ types
- Actor handler dispatch uses a runtime type tag system, not C++ `std::type_index`
- Serialization is automatic via protobuf `SerializeAsString`/`ParseFromString`
- Two dispatch modes: **proto_actor** (protobuf-native) and **typed_actor** (C++-native with protobuf wire)
- Code generation from `.proto` service definitions is optional but supported

## 2. Core Concepts

### 2.1 Message Type System

Three layers of message types exist in the framework:

```
Layer 1: System messages   — completion_msg, down_msg, exit_msg, link_msg, unlink_msg
                             (fixed variant, internal framework use)

Layer 2: User protobuf     — Any protobuf message type defined by the application
messages                    (serialized to bytes, dispatched by TypeTag)

Layer 3: Typed C++         — Application-defined C++ types wrapped with serializers
messages                    (the existing typed_actor<> path)
```

This document focuses on **Layer 2** — protobuf-based user messages.

### 2.2 TypeTag System

Every user message type is identified by a `TypeTag` — a `uint32_t` enum value. The `TypeTag` is the primary dispatch mechanism:

```cpp
// Built-in system tags occupy 0..63
enum class TypeTag : uint32_t {
    Invalid = 0,
    // System types (reserved 1..63):
    CompletionMsg = 1,
    DownMsg = 2,
    ExitMsg = 3,
    LinkMsg = 4,
    UnlinkMsg = 5,
    SpawnRequestTag = 6,
    SpawnResponseTag = 7,
    // User types start at 64:
    User = 64,
};
```

Applications register their protobuf types with sequential tags starting at `User`:

```cpp
// application registration
TypeTag MY_REQUEST_TAG = TypeTag(64);
TypeTag MY_RESPONSE_TAG = TypeTag(65);
```

### 2.3 Wire Format

All user messages on the wire follow this envelope:

```protobuf
// Frame-level envelope (transport layer)
message WireFrame {
    ActorAddress sender = 1;
    ActorAddress receiver = 2;
    uint64 message_id = 3;
    uint32 flags = 4;        // RpcRequest, RpcResponse, etc.
    bytes payload = 5;       // Serialized TypeTag + protobuf payload
}

// Payload format: [4 bytes: TypeTag][protobuf serialized bytes]
```

The `TypeTag` prefix allows the receiver to deserialize the protobuf bytes into the correct type without any C++ RTTI.

---

## 3. User-Defined Actor Base Classes

### 3.1 `proto_actor` — Protobuf-Native Actor

A new actor base class that natively supports protobuf message dispatch:

```
event_based_actor
    └── proto_actor          (NEW: protobuf message handling)
            ├── ProtoStatefulActor<T>       (NEW: stateful + protobuf)
            └── ServiceActor<Service>       (NEW: generated from .proto service)
```

```cpp
class proto_actor : public event_based_actor {
public:
    // Register a handler for a protobuf message type.
    // The handler is invoked when a message with matching TypeTag arrives.
    template<typename ProtoMsgT>
    void on(std::function<void(const ProtoMsgT&)> handler);

    // Register a request handler. The response message is sent back to
    // the sender with the same correlation ID.
    template<typename ReqT, typename ResT>
    void on_request(std::function<ResT(const ReqT&)> handler);

    // Fire-and-forget: send a protobuf message to an actor
    template<typename ProtoMsgT>
    void send(ActorAddress target, const ProtoMsgT& msg);

    // Request-response: send a protobuf request and await a response
    template<typename ReqT, typename ResT>
    void request(ActorAddress target, const ReqT& req,
                 std::function<void(const ResT&)> callback,
                 std::chrono::milliseconds timeout = {});

protected:
    // Users override this to register their handlers.
    // Called once during actor initialization, before the first message.
    virtual void register_handlers() = 0;

    // The framework calls this method to dispatch incoming user messages.
    // It looks up the TypeTag in the handler registry and deserializes.
    void on_proto_message(TypeTag tag, const bytes& payload);

private:
    // Per-type handler registry:
    //   TypeTag → (deserialize_fn, handler_fn)
    std::unordered_map<TypeTag, ProtoHandler> proto_handlers_;
};
```

### 3.2 `ProtoStatefulActor<T>` — Stateful Protobuf Actor

Combines stateful actor pattern with protobuf message handling:

```cpp
template<typename StateT>
class proto_stateful_actor : public proto_actor {
public:
    StateT& state() { return state_; }
    const StateT& state() const { return state_; }

protected:
    // Override register_handlers() to register handlers that can use state()

private:
    StateT state_;
};
```

### 3.3 `ServiceActor<Service>` — Code-Generated Actor (Optional)

For applications that define their service contracts in `.proto` files, code generation creates a type-safe actor base class. This is the highest-level abstraction.

```protobuf
// myapp/echo_service.proto
package myapp;

message EchoRequest {
    string text = 1;
}
message EchoResponse {
    string text = 1;
}

service EchoService {
    rpc Echo(EchoRequest) returns (EchoResponse);
    rpc Notify(EchoRequest) returns (google.protobuf.Empty);
}
```

Generated actor base class:

```cpp
// Generated from echo_service.proto
class EchoServiceActor : public proto_actor {
public:
    // Type-safe handler registration methods
    using EchoHandler = std::function<EchoResponse(const EchoRequest&)>;
    using NotifyHandler = std::function<void(const EchoRequest&)>;

    void set_echo_handler(EchoHandler handler);
    void set_notify_handler(NotifyHandler handler);

protected:
    // register_handlers() is auto-generated to wire up TypeTag dispatch
    void register_handlers() override;
};
```

User implementation:

```cpp
class MyEchoActor : public EchoServiceActor {
protected:
    void register_handlers() override {
        set_echo_handler([this](const EchoRequest& req) -> EchoResponse {
            EchoResponse res;
            res.set_text(req.text());
            return res;
        });

        set_notify_handler([this](const EchoRequest& req) {
            printf("Notify: %s\n", req.text().c_str());
        });
    }
};
```

---

## 4. Handler Dispatch Mechanics

### 4.1 TypeTag Registration

Each protobuf message type must be registered with the actor system before use. This maps TypeTag values to protobuf descriptors for automatic serialization.

```cpp
class ProtoTypeRegistry {
public:
    // Register a protobuf message type with a TypeTag
    template<typename ProtoMsgT>
    void register_type(TypeTag tag, const std::string& type_name);

    // Deserialize bytes to a protobuf message given a TypeTag
    std::unique_ptr<google::protobuf::Message> deserialize(
        TypeTag tag, const bytes& data);

    // Serialize a protobuf message to bytes prefixed with TypeTag
    bytes serialize(TypeTag tag, const google::protobuf::Message& msg);

private:
    struct Entry {
        std::string type_name;
        const google::protobuf::Descriptor* descriptor;
        const google::protobuf::Message* prototype;
    };
    std::unordered_map<TypeTag, Entry> registry_;
};
```

### 4.2 Handler Registration (actor-level)

In `proto_actor::register_handlers()`, the user calls `on<MsgT>()`:

```cpp
void register_handlers() override {
    on<EchoRequest>([this](const EchoRequest& req) {
        // Handle EchoRequest
    });
    on<EchoResponse>([this](const EchoResponse& res) {
        // Handle EchoResponse
    });
    on_request<EchoRequest, EchoResponse>([this](const EchoRequest& req) {
        EchoResponse res;
        res.set_text("echo: " + req.text());
        return res;
    });
}
```

The `on<MsgT>()` method:

1. Retrieves the `TypeTag` for `MsgT` from the actor system's `ProtoTypeRegistry`
2. Stores `(TypeTag, deserialize_fn, handler_fn)` in `proto_handlers_`
3. The `deserialize_fn` is generated from `MsgT::ParseFromString()`

### 4.3 Inbound Message Flow

When a protobuf message arrives at an actor:

```
1. Frame arrives → extract payload bytes
2. Read first 4 bytes → TypeTag
3. Look up TypeTag in proto_handlers_
4. Call deserialize_fn → typed protobuf message
5. Call handler_fn with the typed message
6. If it's a request and handler returns a response:
   a. Create response frame with same message_id
   b. Serialize response with TypeTag prefix
   c. Send back via transport
```

### 4.4 `on_request` Mechanics

The `on_request<ReqT, ResT>()` registration sets up bidirectional message handling:

```cpp
template<typename ReqT, typename ResT>
void on_request(std::function<ResT(const ReqT&)> handler) {
    // Register a handler that:
    // 1. Deserializes ReqT from the payload
    // 2. Calls user's handler to get ResT
    // 3. Serializes ResT with the response TypeTag
    // 4. Sends the response back with the same message_id

    TypeTag req_tag = system().proto_registry().get_tag<ReqT>();
    TypeTag res_tag = system().proto_registry().get_tag<ResT>();

    proto_handlers_[req_tag] = ProtoHandler{
        .type_name = typeid(ReqT).name(),
        .deserialize = [](const bytes& data) -> std::shared_ptr<void> {
            auto msg = std::make_shared<ReqT>();
            msg->ParseFromArray(data.data(), static_cast<int>(data.size()));
            return msg;
        },
        .invoke = [this, handler, res_tag](std::shared_ptr<void> raw_msg,
                                            ActorAddress sender,
                                            uint64_t message_id) {
            auto& req = *static_cast<ReqT*>(raw_msg.get());
            ResT res = handler(req);

            bytes res_bytes(res.ByteSizeLong());
            res.SerializeToArray(res_bytes.data(),
                                 static_cast<int>(res_bytes.size()));

            // Send response back to sender
            context()->reply_with_tag(sender, res_tag, res_bytes, message_id);
        }
    };
}
```

---

## 5. Handler Registration Styles

The user has three options for registering handlers. Each represents a different programming style:

### 5.1 Explicit Lambda Registration

Best for: simple actors, rapid prototyping, small number of handlers.

```cpp
class EchoActor : public proto_actor {
protected:
    void register_handlers() override {
        on<EchoRequest>([this](const EchoRequest& req) {
            printf("Got: %s\n", req.text().c_str());
        });
    }
};
```

### 5.2 Dispatch-to-Method

Best for: actors with many message types, better testability, clear separation.

```cpp
class OrderActor : public proto_actor {
protected:
    void register_handlers() override {
        on<CreateOrder>([this](const CreateOrder& msg) { on_create_order(msg); });
        on<CancelOrder>([this](const CancelOrder& msg) { on_cancel_order(msg); });
        on<GetOrderStatus>([this](const GetOrderStatus& msg) { on_get_status(msg); });
    }

private:
    void on_create_order(const CreateOrder& msg) { /* ... */ }
    void on_cancel_order(const CancelOrder& msg) { /* ... */ }
    void on_get_status(const GetOrderStatus& msg) { /* ... */ }
};
```

### 5.3 Code-Generated Service Actor

Best for: service-oriented APIs, protobuf-first design, cross-language interop.

```cpp
// Generated from .proto service definition
class MyServiceActor : public ServiceActor<myapp::MyService> {
protected:
    // register_handlers() is auto-generated
    // User overrides individual RPC handlers:

    result<GetUserResponse> on_get_user(const GetUserRequest& req) override {
        GetUserResponse res;
        res.set_name("Alice");
        res.set_id(req.user_id());
        return res;
    }
};
```

---

## 6. Message Sending from Proto Actors

### 6.1 Fire-and-Forget

```cpp
void send(ActorAddress target, const ProtoMsgT& msg) {
    bytes payload = encode_proto(tag, msg);

    WireFrame frame;
    frame.sender = address();
    frame.receiver = target;
    frame.message_id = MessageId::generate().value();
    frame.flags = 0;  // no flags = one-way
    frame.payload = payload;

    transport().send(frame);
}
```

### 6.2 Request-Response with Callback

```cpp
template<typename ReqT, typename ResT>
void request(ActorAddress target, const ReqT& req,
             std::function<void(const ResT&)> callback,
             std::chrono::milliseconds timeout) {
    TypeTag req_tag = system().proto_registry().get_tag<ReqT>();
    TypeTag res_tag = system().proto_registry().get_tag<ResT>();

    bytes payload = encode_proto(req_tag, req);

    uint64_t msg_id = MessageId::generate().value();

    // Store callback by message_id
    pending_requests_[msg_id] = PendingRequest{
        .callback = [callback](const bytes& data) {
            ResT res;
            res.ParseFromArray(data.data(), static_cast<int>(data.size()));
            callback(res);
        },
        .timeout = timeout,
        .started_at = Clock::now(),
    };

    WireFrame frame;
    frame.sender = address();
    frame.receiver = target;
    frame.message_id = msg_id;
    frame.flags = WireFrame::RpcRequest;
    frame.payload = payload;

    transport().send(frame);
}
```

### 6.3 Replying to a Request

```cpp
void reply(ActorAddress sender, uint64_t message_id,
           const ProtoMsgT& msg) {
    TypeTag tag = system().proto_registry().get_tag<ProtoMsgT>();
    bytes payload = encode_proto(tag, msg);

    WireFrame frame;
    frame.sender = address();
    frame.receiver = sender;
    frame.message_id = message_id;
    frame.flags = WireFrame::RpcResponse;
    frame.payload = payload;

    transport().send(frame);
}
```

---

## 7. Integration with Existing Infrastructure

### 7.1 Actor Context Integration

`ActorContext` gains protobuf-aware methods:

```cpp
class ActorContext {
    // Existing methods:
    void send(ActorAddress target, MessageVariant msg);
    void reply(MessageVariant msg);

    // New protobuf-aware methods:
    template<typename ProtoMsgT>
    void send_proto(ActorAddress target, const ProtoMsgT& msg);

    template<typename ProtoMsgT>
    void reply_proto(ActorAddress target, const ProtoMsgT& msg);

    template<typename ReqT, typename ResT>
    void request_proto(ActorAddress target, const ReqT& req,
                       std::function<void(const ResT&)> callback);
};
```

### 7.2 ActorSystem Integration

```cpp
class ActorSystem {
    // Existing:
    template<typename T, typename... Args> Actor spawn(Args&&... args);

    // New protobuf registration:
    ProtoTypeRegistry& proto_registry() { return proto_registry_; }

    // Register a protobuf message type with the system
    template<typename ProtoMsgT>
    void register_proto_type(TypeTag tag, const std::string& name);

private:
    ProtoTypeRegistry proto_registry_;
};
```

### 7.3 Serializer Integration

The existing `DefaultSerializer` gains protobuf-aware encode/decode:

```cpp
class DefaultSerializer {
    // Existing:
    bytes encode(TypeTag tag, const MessageVariant& msg);

    // New protobuf methods:
    bytes encode_proto(TypeTag tag, const google::protobuf::Message& msg);
    std::unique_ptr<google::protobuf::Message> decode_proto(
        TypeTag tag, const bytes& data, ProtoTypeRegistry& registry);
};
```

---

## 8. Complete Usage Examples

### 8.1 Echo Actor (Minimal)

```cpp
#include <hpactor/actor/proto_actor.hpp>
#include "echo.pb.h"  // EchoRequest, EchoResponse

using namespace hpactor;

// Register protobuf types
constexpr TypeTag EchoRequestTag(64);
constexpr TypeTag EchoResponseTag(65);

class EchoActor : public proto_actor {
protected:
    void register_handlers() override {
        on<EchoRequest>([this](const EchoRequest& req) {
            printf("Echo: %s\n", req.text().c_str());
        });

        on_request<EchoRequest, EchoResponse>(
            [](const EchoRequest& req) -> EchoResponse {
                EchoResponse res;
                res.set_text("echo: " + req.text());
                return res;
            }
        );
    }
};

int main() {
    ActorSystem system(Config{});
    system.register_proto_type<EchoRequest>(EchoRequestTag, "EchoRequest");
    system.register_proto_type<EchoResponse>(EchoResponseTag, "EchoResponse");

    auto actor = system.spawn<EchoActor>();
    actor->send(actor.address(), EchoRequest{"hello"});

    return 0;
}
```

### 8.2 Stateful Order Actor (Medium)

```cpp
struct OrderState {
    std::unordered_map<std::string, Order> orders;
    int next_id = 1;
};

class OrderActor : public proto_stateful_actor<OrderState> {
protected:
    void register_handlers() override {
        on<CreateOrder>([this](const CreateOrder& msg) {
            Order order;
            order.set_id(state().next_id++);
            order.set_item(msg.item());
            order.set_quantity(msg.quantity());
            state().orders[std::to_string(order.id())] = order;
            printf("Created order %d\n", order.id());
        });

        on_request<GetOrderRequest, GetOrderResponse>(
            [this](const GetOrderRequest& req) -> GetOrderResponse {
                GetOrderResponse res;
                auto it = state().orders.find(req.order_id());
                if (it != state().orders.end()) {
                    *res.mutable_order() = it->second;
                }
                return res;
            }
        );
    }
};
```

### 8.3 Supervision with Proto Actors

```cpp
class WorkerActor : public proto_actor {
protected:
    void register_handlers() override {
        on<WorkRequest>([this](const WorkRequest& req) {
            if (req.data() == "crash") {
                throw std::runtime_error("simulated crash");
            }
            printf("Processing: %s\n", req.data().c_str());
        });
    }
};

class SupervisorActor : public self_supervising_actor {
    void init() {
        auto worker = spawn<WorkerActor>();
        add_child(worker);

        // Send work request as protobuf message
        WorkRequest req;
        req.set_data("hello");
        send(worker.address(), req);
    }

    SupervisionDirective on_failure(ActorId child, const error& err) override {
        return SupervisionDirective::Restart;
    }
};
```

---

## 9. Code Generation from .proto Services (Future)

### 9.1 Protobuf Plugin

A `protoc` plugin generates actor base classes from service definitions:

```protobuf
service UserService {
    rpc CreateUser(CreateUserRequest) returns (CreateUserResponse);
    rpc GetUser(GetUserRequest) returns (GetUserResponse);
    rpc NotifyUser(NotifyRequest) returns (google.protobuf.Empty);
}
```

Generated output:
```cpp
// user_service.actor.h (generated)
class UserServiceActor : public proto_actor {
public:
    // Type-safe convenience methods for sending requests
    void create_user(ActorAddress target, const CreateUserRequest& req);
    void get_user(ActorAddress target, const GetUserRequest& req);
    void notify_user(ActorAddress target, const NotifyRequest& req);

    // Request-response (future-based)
    using CreateUserResult = std::function<void(const CreateUserResponse&)>;
    using GetUserResult = std::function<void(const GetUserResponse&)>;

protected:
    // Auto-generated handler registration
    void register_handlers() override;

    // User overrides these:
    virtual CreateUserResponse on_create_user(const CreateUserRequest& req) = 0;
    virtual GetUserResponse on_get_user(const GetUserRequest& req) = 0;
    virtual void on_notify_user(const NotifyRequest& req) {}
};
```

### 9.2 TypeTag Auto-Assignment

The plugin assigns stable TypeTag values based on proto field numbers or a deterministic hash of the service fully-qualified name:

```cpp
namespace tags {
    constexpr TypeTag CreateUserRequestTag(100);
    constexpr TypeTag CreateUserResponseTag(101);
    constexpr TypeTag GetUserRequestTag(102);
    constexpr TypeTag GetUserResponseTag(103);
    constexpr TypeTag NotifyRequestTag(104);
}
```

---

## 10. Design Rationale

### 10.1 Why `proto_actor` Instead of Extending `event_based_actor`?

**Separation of concerns.** The existing `Behavior`/`message_handler` system uses C++ `std::type_index` to match messages — this works for the fixed `MessageVariant` variant but doesn't extend to protobuf types. A dedicated `proto_actor` base class:

- Avoids coupling protobuf dispatch into the core `event_based_actor`
- Keeps the `TypeTag` dispatch path separate from `std::type_index` dispatch
- Allows the protobuf dependency to be optional (applications that don't use protobuf don't pay for it)

### 10.2 Why `TypeTag` Instead of `std::type_index` for Protobuf Types?

**Deterministic serialization.** `std::type_index` is:
- Compiler-specific (different compilers mangle differently)
- Not portable across process boundaries
- Not stable across builds

`TypeTag` (uint32_t) is:
- Stable across processes and builds
- Trivially serializable (4 bytes on the wire)
- Fast to dispatch (integer switch/map lookup)
- User-controllable (can assign specific ranges for different modules)

### 10.3 Why `register_handlers()` Instead of Constructor Registration?

**Virtual dispatch safety.** Calling `on<MsgT>()` in a constructor is unsafe because virtual functions aren't available during base class construction. A separate `register_handlers()` override:

- Is called after the object is fully constructed
- Can call virtual methods safely
- Follows the same pattern as `make_behavior()` in `event_based_actor`
- Separates initialization from handler registration

### 10.4 Why `on_request` Instead of Separate Send/Receive Methods?

**Bidirectional protocol encapsulation.** In request-response patterns, the handler must:
1. Deserialize the request
2. Execute business logic
3. Serialize the response
4. Send the response back with the correct correlation ID

The `on_request<ReqT, ResT>()` API encapsulates all four steps, ensuring:
- Response messages are always correctly correlated
- Response TypeTags are always consistent with the request type
- The user cannot forget to send the response

---

## 11. Implementation Plan

### Phase 1: ProtoTypeRegistry (system-level)
- `ProtoTypeRegistry` class with `register_type<T>()`, `serialize()`, `deserialize()`
- Integration with `ActorSystem` as `proto_registry_`
- Thread-safe registration and lookup

### Phase 2: ProtoHandler (actor-level)
- `ProtoHandler` struct with deserialize + invoke
- `proto_actor` base class with `on<T>()`, `on_request<ReqT, ResT>()`, `register_handlers()`
- `proto_stateful_actor<T>` with state management

### Phase 3: Message Sending
- `send_proto()` and `reply_proto()` in `ActorContext`
- `request_proto()` with callback and timeout
- Integration with `RpcChannel` for response routing

### Phase 4: Code Generation (Optional)
- `protoc` plugin for generating `ServiceActor` base classes
- TypeTag auto-assignment from `.proto` service definitions
- Integration with `cmake/protobuf.cmake`

### Phase 5: Documentation & Examples
- Echo actor example using proto_actor
- Stateful order processing example
- Supervision with proto actors example

---

## 12. File Structure

```
include/hpactor/
├── actor/
│   ├── proto_actor.hpp           (NEW: proto_actor base class)
│   ├── proto_stateful_actor.hpp  (NEW: stateful proto actor)
│   └── service_actor.hpp         (NEW: generated base, optional)
│
├── core/
│   └── proto_type_registry.hpp   (NEW: TypeTag → protobuf type mapping)
│
├── types/
│   └── serialization.hpp         (extended: encode_proto/decode_proto)
│
└── rpc/
    └── rpc_channel.hpp           (extended: proto-aware request routing)

protos/hpactor/
└── user_message.proto            (NEW: WireProtoEnvelope for user msgs)

tests/
├── actor/test_proto_actor.cpp    (NEW)
└── actor/test_proto_request.cpp  (NEW)
```

---

## 13. Open Questions

- [ ] Should `proto_actor` inherit from `event_based_actor` or be a standalone base class?
- [ ] Maximum TypeTag value range? (uint32_t = 4B, but reserved ranges?)
- [ ] How should protobuf `Any` messages be handled?
- [ ] Should there be a `Behavior` equivalent for protobuf actors (ProtoBehavior)?
- [ ] Error handling for unknown TypeTags at the receiver?
- [ ] Streaming RPC support (server push, bidirectional)?

---

## 14. References

- [Actor Core Concept & Type Hierarchy](actors-data-structure-design.md)
- [Protobuf Serialization Design](../core/protobuf-serialization-design.md) (TBD)
- [Distributed Actor System Architecture](../Distributed-Actor-System-Software-Architecture-and-Key-Concept-Description-High-Level-Design.md)
- [CAF Actor Types](https://github.com/actor-framework/actor-framework)
