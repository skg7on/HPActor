# MessageVariant Protobuf Serialization Design Spec

> **Date:** 2026-04-24
> **Status:** Draft

## Goal

Replace hardcoded `memcpy`-based system message encoding/decoding in `DefaultSerializer` with Protocol Buffers, using `protos/hpactor/messages.proto` as the schema definition.

## Motivation

The current `DefaultSerializer::encode_system` / `decode_system` uses manual `memcpy` with fixed byte offsets for `down_msg`, `exit_msg`, `link_msg`, and `unlink_msg`. This approach has the same problems as the old `WireFrame` encoding:

- No schema evolution — adding fields breaks wire compatibility
- Manual buffer management is error-prone
- No validation — garbage in silently produces garbage out
- Protocol versioning is invasive

The existing `protos/hpactor/frame.proto` handles transport framing (`WireFrame`). This spec adds `protos/hpactor/messages.proto` for application message types.

## Scope

| What | What NOT |
|------|----------|
| System messages: `down_msg`, `exit_msg`, `link_msg`, `unlink_msg` | User message types (`ping_msg`, `pong_msg`, etc.) remain unserialized |
| `SpawnRequest`, `SpawnResponse` via `encode_spawn`/`decode_spawn` | WireFrame transport (already protobuf-ized) |
| C++ structs stay in-memory | C++ structs do NOT become protobuf message classes |

## Design Decisions

### 1. C++ structs remain in-memory

`down_msg`, `exit_msg`, etc. are not replaced by generated protobuf classes. Protobuf is only the serialization layer. This keeps the migration incremental and avoids cascading changes through the actor framework.

### 2. Separate schema from transport

`messages.proto` defines message types separately from `frame.proto` (which handles transport). `DefaultSerializer` produces protobuf bytes stored in `WireFrame.payload`.

```
ActorSystem → DefaultSerializer → protobuf bytes → WireFrame.payload → frame_to_proto → TCP
```

### 3. Big-bang migration

All system message types are migrated in a single PR. No dual-serialization paths.

## Schema Design

Three proto files, with `common.proto` factored out to avoid duplicate type definitions across packages:

```protobuf
// protos/hpactor/common.proto
syntax = "proto3";

package hpactor;

message ActorEndpoint {
  oneof type {
    Ipv4Endpoint ipv4 = 1;
    Ipv6Endpoint ipv6 = 2;
  }
}

message ActorRef {
  ActorEndpoint endpoint = 1;
  uint32 type = 2;
  uint64 actor_id = 3;
  uint64 incarnation = 4;
}

message ActorAddress {
  ActorEndpoint endpoint = 1;
  uint32 type = 2;
  uint64 actor_id = 3;
  uint64 incarnation = 4;
}

message Ipv4Endpoint {
  fixed32 addr = 1;
  uint32 port = 2;
}

message Ipv6Endpoint {
  bytes addr = 1;
  uint32 port = 2;
}
```

```protobuf
// protos/hpactor/messages.proto
syntax = "proto3";

package hpactor;

import "hpactor/common.proto";

message DownMessage {
  ActorEndpoint endpoint = 1;  // terminated_actor.endpoint
  uint64 actor_id = 2;          // terminated_actor.id.value()
  uint32 reason_code = 3;       // reason.code()
}

message ExitMessage {
  ActorEndpoint sender = 1;
  uint64 actor_id = 2;
  uint32 reason_code = 3;
}

message LinkMessage {
  ActorEndpoint target = 1;
  uint64 actor_id = 2;
}

message UnlinkMessage {
  ActorEndpoint target = 1;
  uint64 actor_id = 2;
}

message SpawnRequestMessage {
  string actor_type_name = 1;
  uint32 args_type = 2;               // TypeTag as uint32
  bytes serialized_args = 3;
  ActorRef supervisor = 4;
}

message SpawnResponseMessage {
  ActorAddress actor_addr = 1;
  uint32 error_code = 2;
}
```

> **Note:** `frame.proto` (existing, package `hpactor.net`) must also import `common.proto` and replace its duplicate `ActorAddress`, `Endpoint`, `Ipv4Endpoint`, and `Ipv6Endpoint` definitions. This is a prerequisite refactor — the endpoint types must be shared before `messages.proto` can use them without duplication.

### Field mapping

| C++ field | Proto field | Notes |
|---|---|---|
| `down_msg::terminated_actor.endpoint` | `DownMessage.endpoint` | `ActorEndpoint` |
| `down_msg::terminated_actor.id.value()` | `DownMessage.actor_id` | `uint64` |
| `down_msg::reason.code()` | `DownMessage.reason_code` | `uint32` |
| `exit_msg::sender.endpoint` | `ExitMessage.sender` | `ActorEndpoint` |
| `exit_msg::sender.id.value()` | `ExitMessage.actor_id` | `uint64` |
| `exit_msg::reason.code()` | `ExitMessage.reason_code` | `uint32` |
| `link_msg::target.endpoint` | `LinkMessage.target` | `ActorEndpoint` |
| `link_msg::target.id.value()` | `LinkMessage.actor_id` | `uint64` |
| `unlink_msg::target.endpoint` | `UnlinkMessage.target` | `ActorEndpoint` |
| `unlink_msg::target.id.value()` | `UnlinkMessage.actor_id` | `uint64` |
| `SpawnRequest::actor_type_name` | `SpawnRequestMessage.actor_type_name` | `string` |
| `SpawnRequest::args_type` | `SpawnRequestMessage.args_type` | `uint32` (TypeTag) |
| `SpawnRequest::serialized_args` | `SpawnRequestMessage.serialized_args` | `bytes` |
| `SpawnRequest::supervisor_addr` | `SpawnRequestMessage.supervisor` | `ActorRef` |
| `SpawnResponse::actor_addr` | `SpawnResponseMessage.actor_addr` | `ActorAddress` |
| `SpawnResponse::error_code` | `SpawnResponseMessage.error_code` | `uint32` |

## File Changes

| File | Change |
|------|--------|
| `protos/hpactor/common.proto` | **Create** — shared types (`ActorEndpoint`, `ActorRef`, `ActorAddress`, `Ipv4Endpoint`, `Ipv6Endpoint`) |
| `protos/hpactor/messages.proto` | **Create** — protobuf schema for system messages, imports `common.proto` |
| `protos/hpactor/frame.proto` | **Modify** — remove duplicate endpoint types, import `common.proto` instead (prerequisite) |
| `CMakeLists.txt` | **Modify** — add `common.proto` and `messages.proto` to protobuf generation |
| `src/net/frame.cpp` | **Modify** — update `frame_to_proto`/`frame_from_proto` to use shared types from `common.proto` |
| `include/hpactor/types/serialization.hpp` | **Modify** — no interface change; `encode_system`/`decode_system` keep their names but now use protobuf internally |
| `src/core/serialization.cpp` | **Rewrite** — replace `memcpy` with protobuf serialization for all message types |

## Serialization Flow

```
// Encode
MessageVariant msg = ...;
TypeTag tag = TypeTag::DownMsg;
bytes data = DefaultSerializer::encode_system(msg);  // → protobuf bytes
WireFrame frame = {sender, receiver, data, flags, static_cast<uint32_t>(tag)};
bytes wire = frame.encode();  // → frame_to_proto → TCP

// Decode
bytes wire = receive();
WireFrame frame = WireFrame::decode(wire);  // → frame_from_proto
TypeTag tag = static_cast<TypeTag>(frame.type_tag);
bytes data = frame.payload;
MessageVariant msg = DefaultSerializer::decode_system(tag, data);  // protobuf → MessageVariant
```

The `encode_system`/`decode_system` method names are unchanged — the implementation swaps from `memcpy` to protobuf internally.

## Error Handling

- Malformed protobuf data: return default-constructed `MessageVariant{}` (consistent with existing behavior)
- Unknown TypeTag: return `MessageVariant{}` (same as before)
- Empty data: return `MessageVariant{}`

## Wire Compatibility

This is a **breaking change** to the internal serialization format. Since HPActor has no external users yet and the `CommunicationEndpoint` refactor (commit `e104111`) already changed the wire format, this is acceptable.

## Dependencies

- `protobuf::libprotobuf` — already in CMake via prior `find_package(protobuf)` setup
- `protos/hpactor/common.proto` — generated code (linked into `hpactor_proto`)
- `protos/hpactor/frame.proto` — updated to import `common.proto` (prerequisite)
- `protos/hpactor/messages.proto` — new, imports `common.proto`, added to `hpactor_proto`

## Testing

1. Existing tests that call `encode_system`/`decode_system` continue to work (same semantics, different bytes)
2. Add test: round-trip encode/decode for each message type
3. Add test: malformed data produces default-constructed variant
4. Add test: each `TypeTag` maps to correct protobuf message type

## Alternatives Considered

| Alternative | Why Not |
|------------|---------|
| Replace C++ structs with protobuf classes | Too invasive; requires rewriting all message handlers |
| Keep `memcpy`, add validation layer | Doesn't solve schema evolution or debugging problems |
| MessagePack / CBOR | No schema, no cross-language appeal |
| Cap'n Proto | Different mental model, less familiarity in codebase |
