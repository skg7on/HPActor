# Message Protobuf Unification Design Spec

> **Date:** 2026-04-27
> **Status:** Draft

## Goal

Unify all actor message passing on Protocol Buffers. Every message — system or user, local or remote — is defined in `.proto`, identified by `TypeTag`, and serialized via protobuf. Local delivery uses `shared_ptr<google::protobuf::Message>` for zero-copy dispatch within the same host; remote delivery serializes the same protobuf message into a `WireFrame`.

## Motivation

The current system has two parallel messaging paradigms that overlap awkwardly:

### Current State: Two Paradigms

| | Paradigm A: MessageVariant | Paradigm B: TypedMessage (ProtoActor) |
|---|---|---|
| **Message definition** | C++ structs in `abstract_actor.hpp` | `.proto` files → generated C++ |
| **Type aggregation** | Monolithic `std::variant<...>` | `TypeTag` + `bytes` |
| **System msg serialize** | Protobuf via `messages.proto` ✅ | Protobuf `ParseFromArray`/`SerializeToArray` ✅ |
| **User msg serialize** | `memcpy(sizeof(T))` placeholder ❌ | Protobuf ✅ |
| **User msg deserialize** | **Not implemented** ❌ | Protobuf ✅ |
| **Add new msg type** | Edit `MessageVariant` ❌ | Register in `ProtoTypeRegistry` ✅ |
| **Local delivery** | `MessageVariant` by value | `TypedMessage` stuffed into `MessageVariant` |
| **Handler dispatch** | `std::visit` over variant | `TypeTag` lookup in handler map |

### Specific Problems

1. **`MessageVariant` is closed.** Adding a user message type requires editing the variant in `abstract_actor.hpp` — a central contention point.

2. **User serialization is broken.** `DefaultSerializer::register_user_type<T>()` does raw `memcpy` of `sizeof(T)`. The decode path is commented as "intentionally unused." This is not production-ready.

3. **Two type registries.** `DefaultSerializer` has `encoders_`/`decoders_` maps; `ProtoTypeRegistry` has its own tag-to-prototype map. They serve the same purpose and can diverge.

4. **Local and remote paths are completely different.** Local passes `MessageVariant` through mailboxes; remote goes through `ActorProxy::send()` → `DefaultSerializer::encode()` → `WireFrame`. This means a local-only system can't be tested for wire compatibility without a full network stack.

5. **`ActorProxy::send()` uses `std::visit` with `std::is_same_v` chains** to determine the `TypeTag` from a `MessageVariant`. Every new system message type requires adding another branch.

6. **`ProtoActor` inherits `EventBasedActor` but overrides `receive()`** to intercept `TypedMessage` from `MessageVariant`. Two type systems stapled together via inheritance — fragile and confusing.

7. **System message protobuf schemas are incomplete.** `DownMessage`, `ExitMessage`, etc. in `messages.proto` omit `ActorAddress.type` and `incarnation` fields, so those are zeroed on decode.

## Target Architecture

```
All messages defined in .proto files
         │
         ▼
   google::protobuf::Message (base class)
         │
         ├─ local send ─────────────────────────────┐
         │  ActorContext::send(tag, msg)             │
         │  → TypedMessage(tag, nullptr, msg_ptr)   │
         │  → deliver_local() → mailbox             │
         │  → actor::receive(TypedMessage)          │
         │  → deserialize via msg_ptr->GetTypeName()│
         │     or ParseFromArray(payload) if remote │
         │                                          │
         └─ remote send ────────────────────────────┘
            ActorContext::send(tag, msg)
            → TypedMessage(tag, serialize(msg))
            → WireFrame::encode() → transport
            → remote: WireFrame::decode() → TypedMessage
            → actor::receive(TypedMessage)
            → deserialize via ParseFromArray(payload)
```

### TypedMessage: The Universal Carrier

```cpp
class TypedMessage {
    TypeTag tag_;
    bytes payload_;                              // Always populated (serialized form)
    std::shared_ptr<google::protobuf::Message> parsed_; // Optional local fast path
};
```

- `payload_` is the wire format — always available for forwarding or remote send.
- `parsed_` is the zero-copy fast path for local delivery. When a `shared_ptr<Message>` is passed, it's stored here and `payload_` is populated lazily on first access (or eagerly from the sender's `SerializeToString`).
- A remote receiver only has `payload_`; it deserializes on demand into `parsed_` (lazy or at `receive()` time).

### Message Flow

```
Sender:                                         Receiver:
  auto req = std::make_shared<MyRequest>();        TypedMessage& msg = ...;
  req->set_field(value);                           if (msg.parsed()) {
  context()->send(target, TypeTag::MyReq, req);       auto& req = static_cast<MyRequest&>(*msg.parsed());
      // stores req in TypedMessage.parsed_            // zero-copy local access
      // serializes to TypedMessage.payload_        } else {
      // if local: deliver parsed_ + payload_          MyRequest req;
      // if remote: deliver payload_ only              req.ParseFromArray(msg.payload());
                                                   }
```

## Scope

| What | What NOT |
|------|----------|
| Replace `MessageVariant` with `TypedMessage` everywhere | Transport layer (TCP, UDP, UDS) |
| `TypedMessage` with `shared_ptr<Message>` local fast path | Frame protocol (`frame.proto`) |
| Merge `DefaultSerializer` into `ProtoTypeRegistry` | Registrar protocol (`registrar.proto`) |
| Define all messages (system + users) in `.proto` | Event loop / I/O backend |
| Unify `EventBasedActor` and `ProtoActor` | Spawn protocol (already protobuf) |
| `AbstractActor::receive(TypedMessage)` | RPC channel wire format (already protobuf) |
| `ActorContext::send(tag, Message&)` / `reply(Message&)` | Supervision strategies |
| `ActorRef::send()` simplified | Examples (updated, not redesigned) |
| 61 existing tests updated to new API | |

## Design Decisions

### 1. `TypedMessage` carries both parsed and serialized form

```cpp
class TypedMessage {
public:
    TypedMessage() = default;
    
    // Local send: parsed message + eager serialize
    TypedMessage(TypeTag tag, std::shared_ptr<google::protobuf::Message> msg);
    
    // Remote receive: serialized payload only
    TypedMessage(TypeTag tag, bytes payload);

    TypeTag type_id() const;
    const bytes& payload() const;
    
    // Returns non-null if the message is available in parsed form
    std::shared_ptr<google::protobuf::Message> parsed() const;
    
    // Deserialize on demand (if only payload is available)
    template<typename T>
    std::shared_ptr<T> as() const;

private:
    TypeTag tag_ = TypeTag::Invalid;
    bytes payload_;                                     // Always populated
    mutable std::shared_ptr<google::protobuf::Message> parsed_; // Optional
};
```

**Why:** Local senders pass a `shared_ptr<Message>` — the receiver gets zero-copy access. Remote receivers get `bytes` and deserialize on first access. The `parsed_` field is `mutable` to allow lazy deserialization in `as<T>()`.

### 2. All messages are defined in `.proto` files

System messages move from C++ structs in `abstract_actor.hpp` to generated protobuf classes:

```protobuf
// protos/hpactor/messages.proto (extended)
message DownMessage {
  ActorAddress terminated_actor = 1;  // Full address (was: endpoint + actor_id only)
  uint32 reason_code = 2;
  bytes reason_text = 3;
}

message ExitMessage {
  ActorAddress sender = 1;            // Full address
  uint32 reason_code = 2;
}

message LinkMessage {
  ActorAddress target = 1;            // Full address
}

message UnlinkMessage {
  ActorAddress target = 1;            // Full address
}
```

User messages are defined in their own `.proto` files and registered with `ProtoTypeRegistry`:

```protobuf
// Example: protos/myapp/calculator.proto
message AddRequest {
  int32 a = 1;
  int32 b = 2;
}
message AddResponse {
  int32 result = 1;
}
```

**Why:** Protobuf becomes the single source of truth for all message schemas. Schema evolution, cross-language interop, and validation come for free.

### 3. `ProtoTypeRegistry` becomes the single type registry

`DefaultSerializer` is removed. Its `encode_system`/`decode_system` logic moves into `ProtoTypeRegistry`:

```cpp
class ProtoTypeRegistry {
public:
    // Register a protobuf message type
    template<typename ProtoMsgT>
    TypeTag register_type(std::string type_name);
    
    // Lookup TypeTag for a protobuf type
    template<typename ProtoMsgT>
    TypeTag lookup() const;
    
    // Create a new message instance by TypeTag (prototype pattern)
    std::shared_ptr<google::protobuf::Message> create(TypeTag tag) const;
    
    // Serialize a protobuf message to bytes
    static bytes serialize(const google::protobuf::Message& msg);
    
    // Deserialize bytes to a protobuf message by TypeTag
    std::shared_ptr<google::protobuf::Message> deserialize(TypeTag tag, const bytes& data) const;
    
    // Wire format: [4-byte big-endian TypeTag][protobuf payload]
    bytes encode_wire(TypeTag tag, const google::protobuf::Message& msg);
    TypedMessage decode_wire(const bytes& wire_data);
};
```

Pre-registered system types use well-known tags (0-99 as today). User types use tags >= 100.

**Why:** One registry, one serialization path. No more `encoders_`/`decoders_` maps vs. protobuf prototype map.

### 4. `AbstractActor::receive()` takes `TypedMessage`

```
Before:  virtual void receive(MessageVariant&& msg) = 0;
After:   virtual void receive(TypedMessage& msg) = 0;
```

`EventBasedActor` dispatches by `TypeTag` to registered handlers — absorbing what `ProtoActor` does today. `ProtoActor` as a separate class may vanish.

**Why:** The base class interface reflects the unified type system. Actors that want typed handler dispatch use `EventBasedActor::on<T>()`; actors that want raw access override `receive()`.

### 5. `ActorContext::send()` takes protobuf messages

```
Before:  void send(const ActorAddress& target, MessageVariant msg);
After:   void send(const ActorAddress& target, TypeTag tag,
                   const google::protobuf::Message& msg);

         // Convenience template
         template<typename ProtoMsgT>
         void send(const ActorAddress& target, const ProtoMsgT& msg);
```

Serialization to `bytes` happens inside `ActorContext::send()` via `ProtoTypeRegistry::serialize()`. The `TypedMessage` is constructed with both `shared_ptr<Message>` (for local fast path) and serialized `payload_` (for remote forwarding).

**Why:** Callers never touch `MessageVariant` or raw serialization. They work exclusively with protobuf message objects.

### 6. Local delivery with `shared_ptr` for concurrent safety

```cpp
void ActorSystem::deliver_local(ActorId target_id, TypedMessage msg) {
    // msg.parsed() holds shared_ptr<Message> — safe to share across mailbox boundaries
    auto actor = registry_.lookup(target_id);
    if (actor) {
        actor->mailbox().push(std::move(msg));
    }
}
```

The `shared_ptr<google::protobuf::Message>` inside `TypedMessage` provides:
- **Thread safety:** `shared_ptr` reference counting is atomic — safe to enqueue and dequeue across threads.
- **Zero-copy:** The receiver gets the same parsed object the sender created — no serialize/deserialize cycle for local actors.
- **Immutability:** Protobuf messages are value types; after construction, the receiver reads but does not mutate. If mutation is needed, the receiver copies.

**Why:** `shared_ptr` is the standard C++ mechanism for shared ownership with atomic reference counting. It's the right tool for concurrent mailbox delivery.

### 7. `Message<T>` template is removed

The intrusive `Message<T>` wrapper (which only provides `mpsc_next` for the MPSC mailbox) is replaced by an intrusive link directly on `TypedMessage`:

```cpp
class TypedMessage {
    // ...
    std::atomic<TypedMessage*> mpsc_next{nullptr};
};
```

**Why:** `Message<T>` was a thin wrapper adding only the intrusive link. With `TypedMessage` as the universal carrier, the link belongs on `TypedMessage` directly.

## TypeTag Allocation

| Range | Owner | Examples |
|-------|-------|----------|
| 0 | Invalid | TypeTag::Invalid |
| 1-9 | System lifecycle | DownMsg(1), ExitMsg(2), LinkMsg(3), UnlinkMsg(4) |
| 10-19 | Spawn | SpawnRequestTag(5), SpawnResponseTag(6) |
| 20-49 | Reserved for future system | — |
| 50-99 | Framework extensions | RPC, streaming, etc. |
| 100+ | User messages | Application-defined |

## Implementation Plan

### Phase 1: Extend Protobuf Schemas

1. Update `messages.proto` — add `type` and `incarnation` fields to system messages (or use `ActorAddress` directly instead of individual fields).
2. Remove system message C++ structs from `abstract_actor.hpp` (`down_msg`, `exit_msg`, `link_msg`, `unlink_msg`, `completion_msg`, example types like `ping_msg`, `pong_msg`, etc.).
3. Add `TypedMessage` to `message.hpp` / `types.hpp` with `shared_ptr<Message>` + `bytes` + `mpsc_next`.
4. Update `ProtoTypeRegistry` with `serialize()`/`deserialize()` methods and pre-register system types.

### Phase 2: Replace MessageVariant with TypedMessage

1. Change `AbstractActor::receive()` signature.
2. Change mailboxes to hold `TypedMessage` instead of `Message<MessageVariant>`.
3. Update `ActorContext::send()` / `reply()` to take `TypeTag + const google::protobuf::Message&`.
4. Update `ActorRef::send()` and `ActorProxy::send()`.
5. Remove `MessageVariant`, remove `Message<T>`, remove `DefaultSerializer`.

### Phase 3: Converge Actor Base Classes

1. Move `ProtoActor` handler dispatch (`on<T>()`, `on_request<ReqT, ResT>()`, `on_proto_message()`) into `EventBasedActor`.
2. `ProtoActor` becomes a type alias or is removed entirely.
3. `StatefulActor<T>` and `TypedEventBasedActor` update to new `receive()` signature.

### Phase 4: Update ActorContext, ActorRef, ActorProxy

1. `ActorContext::send()` serializes once, constructs `TypedMessage` with both `shared_ptr` and `payload`.
2. `ActorRef::send()` dispatches: local → `deliver_local(TypedMessage)`, remote → `ActorProxy::send(TypedMessage)`.
3. `ActorProxy::send()` wraps `TypedMessage.payload()` in `WireFrame` — no more `std::visit`.

### Phase 5: Wire Up Local Fast Path

1. `ActorSystem::deliver_local()` pushes `TypedMessage` (with `shared_ptr`) to mailbox.
2. Receiver's `EventBasedActor::receive()` checks `msg.parsed()` — if non-null, dispatches directly; if null, deserializes via `ProtoTypeRegistry`.
3. Ensure `shared_ptr` reference counting is correct across mailbox boundaries.

### Phase 6: Update Tests and Examples

1. All 61 tests: replace `MessageVariant` construction with `TypedMessage` or protobuf message construction.
2. 5 examples: update to use protobuf message types.
3. Add tests for: local `shared_ptr` fast path, remote deserialization path, lazy deserialization, thread safety of `shared_ptr` across mailbox.

## File Changes

| File | Change |
|------|--------|
| `protos/hpactor/messages.proto` | **Modify** — add full `ActorAddress` fields to system messages |
| `include/hpactor/types/types.hpp` | **Modify** — move `TypedMessage` here, add `shared_ptr<Message>` field, add `mpsc_next` |
| `include/hpactor/types/serialization.hpp` | **Remove** — `DefaultSerializer`, `Serializer` interface (merge into `ProtoTypeRegistry`) |
| `include/hpactor/actor/message.hpp` | **Remove** — `Message<T>` template no longer needed |
| `include/hpactor/actor/abstract_actor.hpp` | **Modify** — remove C++ message structs, change `receive()` signature |
| `include/hpactor/actor/event_based_actor.hpp` | **Modify** — absorb `ProtoActor` handler dispatch |
| `include/hpactor/actor/proto_actor.hpp` | **Remove** — merged into `EventBasedActor` |
| `include/hpactor/core/proto_type_registry.hpp` | **Modify** — add `serialize()`/`deserialize()`, pre-register system types |
| `include/hpactor/core/mailbox.hpp` | **Modify** — hold `TypedMessage` instead of `Message<MessageVariant>` |
| `include/hpactor/core/actor_system.hpp` | **Modify** — `deliver_local()` takes `TypedMessage` |
| `include/hpactor/actor_context.hpp` | **Modify** — `send()`/`reply()` take protobuf message references |
| `include/hpactor/ref/actor_ref.hpp` | **Modify** — `send()` takes `TypedMessage` |
| `include/hpactor/ref/actor_proxy.hpp` | **Modify** — `send()` takes `TypedMessage` |
| `src/core/serialization.cpp` | **Remove** — logic moves to `ProtoTypeRegistry` |
| `src/ref/actor_proxy.cpp` | **Modify** — remove `std::visit`, wrap `TypedMessage` directly |
| `src/ref/actor_ref.cpp` | **Modify** — simplified dispatch |
| `tests/**/*.cpp` | **Modify** — update to new API |
| `examples/*.cpp` | **Modify** — update to protobuf message types |

## Testing

1. **System message round-trip** — encode/decode each system message type through `ProtoTypeRegistry`.
2. **User message round-trip** — register, encode, decode a user-defined protobuf message.
3. **Local fast path** — verify `shared_ptr<Message>` identity across send/receive (same pointer).
4. **Remote path** — verify `parsed()` is null on receive, `as<T>()` deserializes correctly.
5. **Interleaved local/remote** — same actor receives both local (has `parsed_`) and remote (only `payload_`) messages.
6. **Thread safety** — concurrent pushes to a mailbox with `shared_ptr` messages.
7. **Malformed data** — garbage protobuf bytes return error/default, not crash.
8. **Unknown TypeTag** — graceful handling of unregistered message types.
9. **Existing test migration** — all 61 tests pass with updated API.
10. **Example migration** — all 5 examples compile and run.

## Migration Strategy

**Big-bang within a single PR.** The changes touch the core `receive()` interface, `MessageVariant`, and `Message<T>` — all of which are referenced everywhere. A gradual dual-path migration would require maintaining both `MessageVariant` and `TypedMessage` dispatch paths simultaneously, which is more error-prone than a single coordinated change.

Since HPActor has no external consumers, the blast radius is contained to the repo itself.

## Alternatives Considered

| Alternative | Why Not |
|-------------|---------|
| Keep `MessageVariant`, add protobuf as serializer only | Two type systems persist — doesn't solve the closed-variant problem |
| Use `std::any` instead of `TypedMessage` | No TypeTag, no serialization path, RTTI-dependent |
| `shared_ptr<void>` for local fast path | Loses type information; receiver must know the type to cast |
| Cap'n Proto instead of protobuf | Different mental model, less tooling, protobuf already used in project |
| Lazy serialization (serialize only when remote) | Local send always has `shared_ptr`, but `payload_` must be populated for remote forwarding — eager serialize at send time is simpler |
| `unique_ptr<Message>` instead of `shared_ptr` | `unique_ptr` cannot be shared across mailbox boundaries; message may have multiple references |

## Open Questions

1. **Should `payload_` be populated eagerly or lazily for local sends?** — Eager is simpler and ensures the message is serializable (catches protobuf errors at send time). Lazy avoids work when the message never leaves the host. Recommendation: eager for now, profile later.

2. **Should `EventBasedActor` keep the `make_behavior()` / `become()` API?** — Behaviors currently match on `MessageVariant`. With `TypedMessage`, behavior matching is by `TypeTag`. The behavior API can be retained, matching on `TypeTag` instead of `std::holds_alternative`.

3. **What about `completion_msg` (I/O completion)?** — This is an internal system message. It can either be a protobuf message or remain a special internal-only type that doesn't go over the wire. Recommendation: define it in `messages.proto` for consistency, but mark it as internal (tag 7).
