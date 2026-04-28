# Unified Message Passing Design

**Date:** 2026-04-28
**Status:** Design
**Author:** HPActor Team

## Context

HPActor currently has a split message passing architecture:
- `ActorContext::send()` handles only local delivery — remote targets are **silently dropped**
- `ActorRef::send()` dispatches correctly on `variant<Actor, ActorProxy>`, but `ActorContext` doesn't use it
- `reply()` and `reply_with_error()` are empty TODO stubs — sender identity is never tracked
- `deliver_local()` ignores `priority` and `deadline_ns` parameters
- `ConnectionPool::on_frame_received()` only handles RPC responses and spawn responses — general remote actor-to-actor message dispatch is unimplemented

This spec defines a unified message passing architecture where the same API path handles both local and remote delivery, with zero-copy `shared_ptr` handoff locally and protobuf serialization remotely.

## Goals

1. **Location transparency** — `context()->send(target, msg)` works regardless of where the target lives
2. **Zero-copy local path** — Messages between same-process actors pass by `shared_ptr`, no serialize/parse
3. **Unified inbound sink** — All messages (local or remote origin) enter through `deliver_local()`
4. **Working reply()** — Sender identity tracked through the message, reply works for both paths
5. **Minimal API change** — Existing `ActorRef::send()` dispatch already works; extend to `ActorContext`

## Non-Goals

- Changing the wire format (existing protobuf `net::Frame` is sufficient)
- Changing the mailbox implementations
- Changing the scheduler model (polling-based; no per-message notify_ready needed)
- Adding new message types beyond `ErrorMsg`
- Changing the RPC channel (already works)

---

## Design

### 1. ActorContext — Resolution and Routing

`ActorContext` gains a reference cache and resolution logic so `send()` handles remote targets.

```
ActorContext {
    ref_cache_: ActorRefCache           // lazy, LRU eviction (max 256)
    current_sender_: ActorAddress       // set on receive, enables reply()
}
```

**`resolve(ActorAddress) → ActorRef`:**
1. Check `ref_cache_` by `target.id` — hot path
2. Query `system_->get_actor(target.id)` — check local registry
3. For non-loopback endpoints: create `ActorProxy(target, system)` — new constructor
4. Insert into cache (with LRU eviction if at capacity)
5. Return `ActorRef`

**`send(ActorRef, TypedMessage)` — primary overload:**
Sets `msg.sender_address_ = owner_.address()` to record who sent the message, then dispatches through `ActorRef::send()` which already handles `variant<Actor, ActorProxy>` correctly. The `sender_address_` is embedded in the message before dispatch — `ActorContext::send()` is the single point where sender identity is captured. For the remote path, `ActorProxy::send()` also sets `frame.sender` independently (the address on the wire); both serve the same purpose for their respective paths.

**`send(ActorAddress, TypedMessage)` — convenience overload:**
Calls `resolve(target)` then delegates to `send(ActorRef, msg)`.

**API:**
```cpp
void send(const ActorRef& target, TypedMessage msg);          // primary
void send(const ActorAddress& target, TypedMessage msg);      // convenience (resolve + cache)
void send(const ActorAddress& target, TypeTag tag, const google::protobuf::Message& msg);
template<typename ProtoMsgT> void send(const ActorAddress& target, const ProtoMsgT& msg);
void send_with_priority(const ActorAddress& target, TypedMessage msg, uint8_t priority, int64_t deadline_ns);
```

### 2. deliver_local() — Single Inbound Sink

`ActorSystem::deliver_local(ActorId, TypedMessage, ...)` is the **only** function that pushes into an actor's mailbox. Both local sends and remote receives flow through it. The current two overloads are preserved:

```cpp
// Primary: full control
void ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                                uint8_t priority = 0,
                                int64_t deadline_ns = INT64_MAX);

// Convenience: delegates to primary with defaults
void ActorSystem::deliver_local(ActorId target, TypedMessage msg);
```

**Changes from current:**
- `priority` and `deadline_ns` parameters are forward-declared (still accepted, wired to scheduler later when scheduler supports them)
- Sender identity is read from `msg.sender_address()` — already set by the caller (`ActorContext::send()` for local, `deliver_remote()` for remote). `deliver_local()` does NOT take a sender parameter — the message carries its own sender.
- No `notify_ready()` call — the scheduler model remains polling-based (actors self-reactivate while mailbox is non-empty). This is unchanged from current behavior.

**Implementation:**
```cpp
void ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                                uint8_t /*priority*/, int64_t /*deadline_ns*/) {
    auto* mailbox = get_mailbox(target);
    if (!mailbox) return;
    mailbox->push(std::move(msg));
}
```

### 3. deliver_remote() — Wire-to-Mailbox Bridge

New method bridging the transport layer to the unified mailbox. It sets `sender_address_` from the wire frame before calling `deliver_local()`:

```cpp
void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    TypedMessage msg(static_cast<TypeTag>(frame.type_tag), frame.payload);
    msg.set_sender_address(frame.sender);
    deliver_local(frame.receiver.id, std::move(msg));
}
```

This replaces the TODO in `ConnectionPool::on_frame_received()`. The existing RPC response and spawn response handling in `on_frame_received()` stays as special cases (they don't go to actor mailboxes — RPC responses go to the RPC handler, spawn responses go to the spawn handler).

### 4. Sender Tracking and reply()

**New field in `TypedMessage`:**
```cpp
ActorAddress sender_address_{};   // set by caller before deliver_local()
const ActorAddress& sender_address() const;
void set_sender_address(const ActorAddress& addr);
```

The move constructor and move assignment must transfer `sender_address_`.

**On receive — `EventBasedActor::receive(msg)`:**
```cpp
context_->current_sender_ = msg.sender_address();
// ... dispatch by TypeTag ...
```

This resolves the existing `// TODO: integrate with reply routing` comment in `EventBasedActor::receive()`.

**`reply(TypedMessage msg)`:**
```cpp
void ActorContext::reply(TypedMessage msg) {
    if (current_sender_.id != ActorId{0}) {
        send(current_sender_, std::move(msg));
    }
    // No current sender → no-op (e.g., called in constructor)
}
```

**`reply_with_error(error err)`:**
Serializes the error into a standard error payload, sends to `current_sender_` with `TypeTag::ErrorMsg`. The error payload format is a simple protobuf message with error code + string fields.

**New `TypeTag` entry:**
```cpp
enum class TypeTag : uint32_t {
    // ... existing ...
    SpawnResponseTag = 6,
    ErrorMsg = 7,          // NEW: error response
    User = 100,
};
```

### 5. ActorProxy — New Constructor

`ActorProxy` currently takes `ActorAddress` + `Transport*`. A new convenience constructor accepts `ActorAddress` directly and resolves the transport from `ActorSystem` via `get_transport_for()`:

```cpp
// New: for ActorContext::resolve()
ActorProxy::ActorProxy(const ActorAddress& addr, ActorSystem* system)
    : address_(addr), transport_(system->get_transport_for(addr.endpoint)) {}
```

`ActorSystem::get_transport_for()` looks up or creates the `TcpTransport` for the given endpoint. This method must be implemented before this constructor can be used (see implementation order, step 11 before step 12).

### 6. Error Handling

| Scenario | Behavior |
|----------|----------|
| Remote connection down | Queue in `ConnectionPool` pending buffer (existing). If pool is shutting down, deliver error to sender's mailbox |
| Target actor terminated | `deliver_local()` silently drops (mailbox lookup returns null). Matches async messaging semantics |
| Send to self | `deliver_local()` to own mailbox, scheduler picks up next turn (already works) |
| `reply()` with no sender | No-op (current_sender_ is default-invalid) |
| Ref cache full | LRU eviction. Proxy objects are lightweight — eviction is cheap; re-request creates a new one |
| Serialization failure | Caught at `ActorProxy::send()` level, error returned immediately |
| Unknown `TypeTag` on receive | Dropped with warning. User types must be registered in `ProtoTypeRegistry` before use |

### 7. Ref Cache Design

```cpp
class ActorRefCache {
    static constexpr size_t kMaxEntries = 256;

    struct Entry {
        ActorRef ref;
        uint64_t last_access_tick;  // monotonic counter for LRU
    };

    std::unordered_map<ActorId, Entry> cache_;
    uint64_t tick_ = 0;

    std::optional<ActorRef> get(ActorId id);
    void put(ActorId id, ActorRef ref);
    void evict_lru();  // scan for lowest last_access_tick
};
```

Accessed only from the actor's thread — no locking needed.

---

## Data Flow Summary

### Local Send (Same Process)

```
context()->send(target_addr, msg)
  → msg.sender_address_ = owner_.address()
  → resolve(target_addr) → ActorRef(Actor)
  → ActorRef::send() → is_local() = true
  → system().deliver_local(target.id, msg)
  → mailbox->push(msg)  // shared_ptr preserved, sender_address_ set
  → actor wakes, receive(msg)
  → context->current_sender_ = msg.sender_address()
  → msg.as<T>() → static_pointer_cast  // zero-copy
```

### Remote Send (Cross-Host)

```
context()->send(target_addr, msg)
  → msg.sender_address_ = owner_.address()
  → resolve(target_addr) → ActorRef(ActorProxy)
  → ActorRef::send() → is_local() = false
  → proxy->send(target, msg)
  → WireFrame{...}.encode() → protobuf bytes
  → TcpTransport::send() → ConnectionPool → TCP
```

### Remote Receive (Cross-Host)

```
TCP → ConnectionPool::on_frame_received(bytes)
  → WireFrame::decode(bytes)
  → system().deliver_remote(frame)
  → msg.sender_address_ = frame.sender
  → deliver_local(receiver.id, msg)
  → mailbox->push(msg)  // bytes only, no shared_ptr
  → actor wakes, receive(msg)
  → context->current_sender_ = msg.sender_address()
  → msg.as<T>() → ParseFromArray  // one-time lazy parse, cached
```

---

## Implementation Order

1. **Add `sender_address_` to `TypedMessage`** — field, accessors, move constructor/assignment update
2. **Implement `ActorRefCache`** — new header, simple LRU
3. **Add `resolve()` to `ActorContext`** — uses cache + system registry
4. **Rewrite `ActorContext::send()`** — set `msg.sender_address_`, route through `resolve()` + `ActorRef::send()`
5. **Implement `deliver_remote()`** — set `sender_address_` from frame, call `deliver_local()`
6. **Wire `on_frame_received()`** — call `deliver_remote()` for all non-RPC/non-spawn frames
7. **Set `current_sender_` in `receive()`** — extract from `msg.sender_address()`
8. **Implement `reply()` and `reply_with_error()`** — send to `current_sender_`
9. **Add `ErrorMsg` TypeTag** — new system message type + error payload protobuf
10. **Add `get_transport_for()` to `ActorSystem`** — endpoint → TcpTransport mapping
11. **Add `ActorProxy(address, system)` constructor** — convenience for resolve()

---

## Files Changed

| File | Change |
|------|--------|
| `include/hpactor/actor/typed_message.hpp` | Add `sender_address_` field, accessors, update move ctor/assign |
| `include/hpactor/actor_context.hpp` | Add `ref_cache_`, `current_sender_`, new send/reply signatures |
| `src/actor/actor_context.cpp` | Implement `resolve()`, rewrite `send()`, implement `reply()` |
| `include/hpactor/core/actor_system.hpp` | Add `deliver_remote()`, `get_transport_for()` |
| `src/actor/actor_system.cpp` | Implement `deliver_remote()` |
| `include/hpactor/ref/actor_proxy.hpp` | Add `ActorProxy(ActorAddress, ActorSystem*)` constructor |
| `src/ref/actor_proxy.cpp` | Implement new constructor |
| `include/hpactor/types/types.hpp` | Add `ErrorMsg = 7` to TypeTag |
| `src/actor/event_based_actor.cpp` | Set `current_sender_` in `receive()`, resolve reply TODO |
| `src/net/connection_pool.cpp` | Generalize `on_frame_received()` dispatch |
| `include/hpactor/core/actor_ref_cache.hpp` | **New file** — LRU cache for resolved ActorRefs |

Note: `deliver_local()` signature stays the same (4 params). No scheduler contract change — `notify_ready()` is not added to `deliver_local()`. Priority/deadline params remain accepted but not yet acted upon (forward-looking).

## Tests

| Test | What It Validates |
|------|-------------------|
| `test_actor_ref_cache` | LRU insert, lookup, eviction, max size behavior |
| `test_actor_context_resolve` | Local resolve, remote resolve via proxy, cache hit, stale eviction |
| `test_actor_context_send` | Local send reaches mailbox, remote send calls proxy, sender_address_ set |
| `test_actor_context_reply` | Reply uses current_sender_, reply-with-no-sender is no-op |
| `test_reply_with_error` | error → ErrorMsg TypeTag → sender receives it |
| `test_deliver_remote` | WireFrame → TypedMessage → sender_address_ set → deliver_local called |
| `test_unified_integration` | Full loop: send → mailbox → receive → reply → back to original sender |
| `test_cross_host_dispatch` | Simulated remote frame → on_frame_received → deliver_remote → mailbox |
