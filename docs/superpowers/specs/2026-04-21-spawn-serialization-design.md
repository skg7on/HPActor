# Phase 8: Full Serialization Integration for SpawnRequest/SpawnResponse

**Date:** 2026-04-21
**Status:** Draft
**Dependencies:** Phase 6 (Remote Actor Spawn), Phase 7 (Async RPC Channel), Supervision (OneForOne, AllForOne, SelfSupervisingActor)

## Overview

Integrate `SpawnRequest` and `SpawnResponse` into the `MessageVariant` serialization system so they can be encoded/decoded via `DefaultSerializer` and routed through the transport layer using `Frame.message_id` for response correlation.

This enables the full end-to-end spawn flow with hierarchical supervision:
1. Caller (supervisor) serializes `SpawnRequest` via `DefaultSerializer::encode()`
2. `SpawnRequest` carries `supervisor_addr` for link establishment
3. `SpawnRequest` sent over TCP inside a `Frame` with `RpcRequest` flag
4. Remote node deserializes via `DefaultSerializer::decode()`
5. `SpawnReceiver` handles request, creates actor, establishes parent-child link
6. `SpawnResponse` sent back with `RpcResponse` flag and same `MessageId`
7. Hybrid routing: `TypeTag::SpawnResponse` → `pending_spawns_`, other RPC → `RpcChannel`

## Architecture

### MessageVariant Changes

Add `SpawnRequest` and `SpawnResponse` to the `MessageVariant` type alias in `types/types.hpp`:

```cpp
using MessageVariant = std::variant<
    down_msg,
    exit_msg,
    link_msg,
    unlink_msg,
    SpawnRequest,
    SpawnResponse
>;
```

### TypeTag Values

Assign new system TypeTag values in `types/types.hpp`:

```cpp
enum class TypeTag : uint32_t {
    Invalid = 0,

    // System messages (always present)
    DownMsg = 1,
    ExitMsg = 2,
    LinkMsg = 3,
    UnlinkMsg = 4,

    // Spawn protocol (Phase 8)
    SpawnRequestTag = 5,
    SpawnResponseTag = 6,

    // First available user tag
    User = 100,
};
```

### DefaultSerializer Integration

Add encode/decode cases for `SpawnRequest` and `SpawnResponse` in `src/core/serialization.cpp`:

```cpp
// In encode_system():
else if (std::holds_alternative<SpawnRequest>(msg)) {
    // Serialize SpawnRequest: [4b name len][name][4b args_type][4b args len][args]
}
else if (std::holds_alternative<SpawnResponse>(msg)) {
    // Serialize SpawnResponse: [4b node_id][8b actor_id][4b incarnation][4b error_code]
}

// In decode_system():
case TypeTag::SpawnRequestTag: return decode_spawn_request(data);
case TypeTag::SpawnResponseTag: return decode_spawn_response(data);
```

## Wire Format

### SpawnRequest Encoding

```
[4 bytes: name length (uint32_t, big-endian)]
[name length bytes: actor type name]
[4 bytes: args_type (TypeTag as uint32_t)]
[4 bytes: args length (uint32_t)]
[args length bytes: serialized constructor arguments]
[4 bytes: supervisor node_id]
[8 bytes: supervisor actor_id.value()]
[4 bytes: supervisor incarnation]
[4 bytes: supervisor actor_type]
```

### SpawnResponse Encoding

```
[4 bytes: receiver node_id]
[8 bytes: receiver actor_id.value()]
[4 bytes: receiver incarnation]
[4 bytes: actor type]
[4 bytes: error_code]
```

When error_code != 0, actor address fields may be zero/undefined.

## Hierarchical Supervision

### Overview

When a local actor spawns a remote actor, the local actor becomes the supervisor of the newly spawned actor. This requires:

1. **Supervisor address propagation** — `SpawnRequest` carries the supervisor's `ActorAddress`
2. **Link establishment** — When spawn response arrives, local side creates link to remote actor
3. **Child tracking** — Supervisor maintains list of children, including remote children
4. **Exit propagation** — If remote child dies, `down_msg` flows back to local supervisor

### ActorAddress in SpawnRequest

The `SpawnRequest` struct must include the supervisor's address:

```cpp
struct SpawnRequest {
    std::string actor_type_name;    // e.g., "calculator"
    TypeTag args_type;            // type tag for deserializing args
    bytes serialized_args;         // type-erased constructor arguments
    ActorAddress supervisor_addr;  // NEW: supervisor's address for link establishment
};
```

### Link Setup Flow

```
Local Node (Actor A, supervisor)          Remote Node (SpawnReceiver)
        |                                          |
        | --- SpawnRequest ----------------------> |
        |     supervisor_addr = A's address       |
        |     actor_type_name = "worker"           |
        |                                          |
        |                              [SpawnReceiver spawns Worker]
        |                              [SpawnReceiver links to Worker]
        |                              [Worker is child of SpawnReceiver]
        |                                          |
        | <-- SpawnResponse --------------------- |
        |     actor_addr = Worker on remote        |
        |     error_code = 0                       |
        |                                          |
[Actor A receives SpawnResponse]
[Actor A creates link to Worker via ActorProxy]
[Actor A adds Worker to children_ list]
```

Note: Actor A is the logical supervisor, but ActorProxy does not directly track children. Instead:

1. **Local tracking** — `ActorContext` keeps `children_` vector. When spawn succeeds, `ActorContext::add_child()` is called with the remote `ActorRef`.

2. **Remote link** — `ActorProxy::send()` sends a `link_msg` to the remote actor on first link establishment.

3. **Exit propagation** — If remote Worker dies, `down_msg` is sent to Actor A (the supervisor). Actor A's behavior handles `down_msg` and applies restart policy if configured.

### SelfSupervisingActor with Remote Children

`SelfSupervisingActor` manages its own children with a configurable strategy:

```cpp
class SelfSupervisingActor : public EventBasedActor {
    SupervisorPolicy policy_;  // OneForOne or AllForOne
    std::vector<ActorRef> children_;
    std::vector<ActorAddress> child_addresses_;  // For remote children
    // ...
};
```

When spawning remote actor:
1. Add remote `ActorRef` to `children_`
2. Add remote `ActorAddress` to `child_addresses_` (for persistence)
3. Link to remote actor (exchange `link_msg`)
4. If `policy_ == OneForOne`: only failed child restarts
5. If `policy_ == AllForOne`: all children restart if one fails

### Restart Logic for Remote Children

When `SelfSupervisingActor` receives `down_msg` from a remote child:

```cpp
if (down_msg.reason.code() != 0) {  // Non-zero = abnormal exit
    switch (policy_) {
    case SupervisorPolicy::OneForOne:
        // Restart only failed child
        spawn_remote_child(child_address);
        break;
    case SupervisorPolicy::AllForOne:
        // Restart all children
        for (auto& addr : child_addresses_) {
            spawn_remote_child(addr);
        }
        break;
    }
}
```

### Cross-Node Supervision Considerations

**Challenge**: Remote child is on a different node. If supervisor restarts it, the restart request must go through the network.

**Solution**: `SelfSupervisingActor` stores child addresses in `child_addresses_`. When a remote child dies:
1. `down_msg` arrives via network
2. Supervisor checks `child_addresses_` to find the failed child
3. Supervisor sends new `SpawnRequest` with same `actor_type_name` to same remote node
4. Remote node creates new actor, responds with new `ActorAddress`

**Link persistence**: Since `ActorAddress` includes incarnation, a restarted actor on the same node will have a different incarnation. The supervisor updates `child_addresses_` on each successful spawn.

## Frame Usage for Spawn

Spawn uses the existing `Frame` structure with hybrid RPC routing:

```cpp
net::Frame frame;
frame.sender = caller_actor_addr;          // Supervisor's address for response routing
frame.receiver = ActorAddress{             // SpawnReceiver on remote node
    remote_node_id,
    SystemActorType,
    SpawnReceiverId,
    0
};
frame.message_id = MessageId::generate().value();
frame.flags = net::Frame::RpcRequest;      // Hybrid routing
frame.payload = serializer.encode(TypeTag::SpawnRequestTag, spawn_request);
```

Response:

```cpp
net::Frame response;
response.sender = ActorAddress{remote_node_id, SystemActorType, SpawnReceiverId, 0};
response.receiver = caller_actor_addr;    // Supervisor's address
response.message_id = original_message_id; // Correlate with request
response.flags = net::Frame::RpcResponse;
response.payload = serializer.encode(TypeTag::SpawnResponseTag, spawn_response);
```

## Hybrid Response Routing

When `ConnectionPool::on_frame_received()` processes an incoming `Frame`:

```cpp
void ConnectionPool::on_frame_received(const bytes& frame_data) {
    net::Frame frame = net::Frame::decode(frame_data);

    if (frame.flags & net::Frame::RpcResponse) {
        // Check TypeTag in payload to determine routing
        TypeTag tag = extract_type_tag(frame.payload);

        if (tag == TypeTag::SpawnResponseTag) {
            // Route to pending spawns via message_id
            auto it = pending_spawns_.find(frame.message_id);
            if (it != pending_spawns_.end()) {
                SpawnResponse resp = deserialize_spawn_response(frame.payload);
                it->second->set_response(resp);
                pending_spawns_.erase(it);
                return;
            }
        }

        // Fall through to RPC handler for other responses
        if (rpc_handler_) {
            rpc_handler_(frame.message_id, frame.payload);
        }
    }
}
```

This requires `ConnectionPool` to hold a reference to `ActorSystem`'s `pending_spawns_` map, or a callback to route responses.

## Components to Modify

### 1. `include/hpactor/types/types.hpp`

- Extend `TypeTag` enum with `SpawnRequestTag = 5`, `SpawnResponseTag = 6`

### 2. `include/hpactor/types/types.hpp` (or new file)

- Add `MessageVariant` type alias including spawn types

### 3. `src/core/serialization.cpp`

- Add `encode_system()` cases for `SpawnRequest` and `SpawnResponse`
- Add `decode_system()` cases for `SpawnRequestTag` and `SpawnResponseTag`

### 4. `include/hpactor/spawn.hpp`

- Update `SpawnRequest` to include `ActorAddress supervisor_addr`
- Ensure `SpawnRequest` and `SpawnResponse` are self-contained

### 5. `include/hpactor/net/connection_pool.hpp`

- Add `set_spawn_handler()` method or use existing RPC handler with type-tag dispatch
- Store reference to `pending_spawns_` map for response routing

### 6. `src/actor/spawn_receiver.cpp`

- Update `handle_spawn_request()` to deserialize args using `args_type`
- Link to spawned actor (establish child relationship on remote side)
- Send response via transport using `Frame.sender` as reply destination

### 7. `src/actor/actor_system.cpp`

- `spawn_remote_async()`: serialize `SpawnRequest` using `DefaultSerializer`, include supervisor address, store `AsyncActor` in `pending_spawns_`, send via transport

### 8. `src/net/connection_pool.cpp`

- Update `on_frame_received()` to dispatch `SpawnResponse` to pending spawns

### 9. `include/hpactor/actor/self_supervising_actor.hpp`

- Track remote children in `child_addresses_`
- On `down_msg` from remote child, apply restart policy

### 10. `src/actor/actor_context.cpp`

- `add_child(ActorRef remote_child)` — add remote ActorRef to children list

## Implementation Tasks

1. **Update TypeTag enum** — Add `SpawnRequestTag`, `SpawnResponseTag` to `types.hpp`

2. **Add MessageVariant type alias** — Include spawn types in variant in `types.hpp`

3. **Update SpawnRequest struct** — Add `ActorAddress supervisor_addr`

4. **Implement spawn serialization** — Add encode/decode for `SpawnRequest` and `SpawnResponse` in `serialization.cpp`

5. **Update ActorSystem spawn methods** — Serialize `SpawnRequest` properly, use `DefaultSerializer::encode()`, include supervisor address

6. **Wire response routing** — Pass `pending_spawns_` to `ConnectionPool`, route `SpawnResponse` by `MessageId`

7. **Update SpawnReceiver** — Deserialize args, establish link to spawned actor on remote side

8. **Update SelfSupervisingActor** — Track remote children, apply restart policy on `down_msg`

9. **Update ActorContext** — Add `add_child(ActorRef)` for remote children

10. **Add unit tests** — Test spawn serialization round-trip, test response routing, test supervision with remote children

## Testing Approach

### Unit Tests

- `test_spawn_serialization.cpp` — Test `SpawnRequest`/`SpawnResponse` encode/decode round-trip
- `test_spawn_routing.cpp` — Test response routing via message_id
- `test_remote_child_supervision.cpp` — Test OneForOne/AllForOne with remote children

### Integration Test

- `test_remote_spawn_integration.cpp` — Two-process spawn test:
  - Node B: registers `WorkerActor`, listens on port
  - Node A: spawns `WorkerActor` on Node B (A is supervisor)
  - Node A links to Worker
  - Node B kills Worker
  - Verify `down_msg` arrives at A
  - Verify A restarts Worker on Node B (if OneForOne)

## Files to Modify

| File | Change |
|------|--------|
| `include/hpactor/types/types.hpp` | Add `SpawnRequestTag`, `SpawnResponseTag` to TypeTag enum; add MessageVariant type alias |
| `include/hpactor/spawn.hpp` | Add `supervisor_addr` to `SpawnRequest` |
| `src/core/serialization.cpp` | Add encode/decode cases for SpawnRequest/SpawnResponse |
| `include/hpactor/net/connection_pool.hpp` | Add spawn response routing support |
| `src/net/connection_pool.cpp` | Implement hybrid response dispatch |
| `src/actor/spawn_receiver.cpp` | Use DefaultSerializer, establish link to spawned actor |
| `src/actor/actor_system.cpp` | Serialize SpawnRequest with supervisor address, manage pending_spawns_ |
| `include/hpactor/actor/self_supervising_actor.hpp` | Track remote children, handle restart |
| `src/actor/actor_context.cpp` | Add `add_child(ActorRef)` overload for remote children |

## Out of Scope

- Argument deserialization (passing constructor args through spawn) — tracked separately
- Typed RPC API — tracked separately
- Distributed supervision (supervisor on different node than children) — tracked separately